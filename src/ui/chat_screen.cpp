#include "ui/chat_screen.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>

#include "llm/summarizer.hpp"
#include "ui/logo.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/spinner.hpp"
#include "ui/text_wrap.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

namespace {

int count_lines(const std::string& s) {
    if (s.empty()) return 0;
    return 1 + static_cast<int>(std::count(s.begin(), s.end(), '\n'));
}

// Collapses the reasoning to one line for the live preview: newlines become
// spaces so a multi-line tail doesn't shove the input box around.
std::string one_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    return out;
}

}  // namespace

ChatScreen::ChatScreen(AppContext& ctx) : ctx_(ctx) { build_ui(); }

ChatScreen::~ChatScreen() {
    if (worker_.joinable()) worker_.join();
}

void ChatScreen::clear_conversation() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sending_) return;  // a reply is still on its way into this transcript
    conversations_[mode_].clear();
    status_message_.clear();
}

bool ChatScreen::handle_thinking_click(int x, int y) {
    // Ignore clicks outside the transcript: a box belonging to a message that
    // has scrolled out of view can still sit over another part of the screen.
    if (!pane_box_.Contain(x, y)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& conv = conversations_[mode_];
    const size_t n = std::min(conv.size(), thinking_boxes_.size());
    for (size_t i = 0; i < n; ++i) {
        if (conv[i].thinking.empty()) continue;
        if (!thinking_boxes_[i].Contain(x, y)) continue;
        conv[i].thinking_open = !conv[i].thinking_open;
        return true;
    }
    return false;
}

void ChatScreen::send() {
    std::string message = input_text_;
    if (message.find_first_not_of(" \t\r\n") == std::string::npos) return;  // nothing but whitespace

    const std::string model = ctx_.settings.ollama_model;
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "No Ollama model selected -- pick one in Settings.";
        return;
    }

    const int mode = mode_;
    // Snapshotted before the new message is appended: build_chat_prompt takes
    // the thread *up to* this turn, with the turn itself passed separately.
    std::vector<llm::ChatTurn> history;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (sending_) return;  // one request in flight at a time
        if (mode == kChat) {
            for (const auto& msg : conversations_[kChat]) history.push_back({msg.is_user, msg.text});
        }
        Message user_message;
        user_message.is_user = true;
        user_message.text = message;
        conversations_[mode].push_back(std::move(user_message));
        status_message_.clear();
        progress_ = {};
        send_started_ = std::chrono::steady_clock::now();
        sending_ = true;
    }
    input_text_.clear();

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, message, model, mode, history = std::move(history)] {
        // Called from this thread on every streamed chunk; the render loop
        // (app.cpp ticks every 80ms) picks the numbers up on its next pass.
        llm::StreamCallback on_progress = [this](const llm::StreamProgress& p) {
            std::lock_guard<std::mutex> lock(mutex_);
            progress_ = p;
        };

        llm::ChatReply reply;
        std::string error_text;
        try {
            reply = (mode == kProofread)
                        ? llm::proofread_email(ctx_.settings.ollama_host, model, message, on_progress)
                        : llm::chat_reply(ctx_.settings.ollama_host, model, history, message,
                                          ctx_.settings.user_display_name, on_progress);
        } catch (const std::exception& e) {
            error_text = e.what();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (error_text.empty()) {
            Message assistant;
            assistant.is_user = false;
            assistant.text = reply.content;
            assistant.thinking = reply.thinking;
            conversations_[mode].push_back(std::move(assistant));
            status_message_.clear();
        } else {
            status_message_ = "Failed: " + error_text;
        }
        sending_ = false;
    });
}

void ChatScreen::build_ui() {
    using namespace ftxui;

    auto mode_dropdown = Dropdown(&mode_labels_, &mode_);

    InputOption input_opt;
    input_opt.multiline = true;
    // Deliberately no on_enter -> send binding: multiline Input inserts a
    // newline AND fires on_enter on every Return, which would send
    // mid-paste/mid-compose. Submission is via the explicit Send button.
    auto input_box = WrappedInput(&input_text_, "Type a message, or paste an email to proofread...", input_opt);
    auto send_button = Button("Send", [this] { send(); }, ButtonOption::Animated(theme::kAccent));
    auto clear_button = Button("Clear", [this] { clear_conversation(); }, ButtonOption::Animated(theme::kMuted));

    auto chat_pane = ScrollableContent(
        [this](int width) {
            std::vector<Message> conv_copy;
            int mode_copy = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                mode_copy = mode_;
                conv_copy = conversations_[mode_];
                thinking_boxes_.assign(conv_copy.size(), Box());
            }
            // The bubble caps at 76 columns including its border, so its text
            // gets 74 -- less when the pane itself is narrower than that.
            const int bubble_width = std::max(1, std::min(74, width - 2));
            Elements bubbles;
            for (size_t i = 0; i < conv_copy.size(); ++i) {
                const Message& msg = conv_copy[i];
                Elements bubble_parts;
                bubble_parts.push_back(text(msg.is_user ? "You" : "CallScribe") | bold |
                                       color(msg.is_user ? theme::kAccent : theme::kSuccess));

                if (!msg.thinking.empty()) {
                    // The fold: a clickable summary row, and the reasoning
                    // itself only when it is open. reflect() records where the
                    // row landed so the click can be hit-tested (see
                    // handle_thinking_click).
                    const std::string caret = msg.thinking_open ? "▾" : "▸";
                    const std::string label = caret + " thinking (" + std::to_string(count_lines(msg.thinking)) +
                                              " lines) " + (msg.thinking_open ? "-- click to hide" : "");
                    bubble_parts.push_back(text(label) | color(theme::kWarning) | dim |
                                           reflect(thinking_boxes_[i]));
                    if (msg.thinking_open) {
                        bubble_parts.push_back(WrappedText(msg.thinking, std::max(1, bubble_width - 2)) | dim |
                                               borderStyled(ROUNDED, theme::kWarning));
                    }
                }

                bubble_parts.push_back(WrappedText(msg.text, bubble_width) | borderStyled(ROUNDED, theme::kMuted));

                Element bubble_body = vbox(std::move(bubble_parts)) | size(WIDTH, LESS_THAN, 76);
                bubbles.push_back(msg.is_user ? hbox({filler(), bubble_body}) : hbox({bubble_body, filler()}));
            }
            if (bubbles.empty()) {
                // Empty state: the wordmark, the way Claude Code greets you.
                bubbles.push_back(text(""));
                bubbles.push_back(LogoBanner(width, mode_copy == kProofread
                                                        ? "Paste an email below and press Send for a corrected version."
                                                        : "Ask anything -- this thread is remembered until you Clear it."));
            }
            return vbox(bubbles);
        },
        [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return static_cast<int>(conversations_[mode_].size());
        });

    auto layout = Container::Vertical({mode_dropdown, chat_pane, input_box, send_button, clear_button});

    root_ = Renderer(layout, [this, chat_pane, input_box, send_button, clear_button, mode_dropdown] {
        std::string status_copy;
        bool sending_now = false;
        int mode_copy = 0;
        llm::StreamProgress progress_copy;
        std::chrono::steady_clock::time_point started;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_copy = status_message_;
            sending_now = sending_;
            mode_copy = mode_;
            progress_copy = progress_;
            started = send_started_;
        }

        Elements children;
        children.push_back(hbox({
            text("CallScribe -- Chat") | bold | color(theme::kAccent),
            text("   Mode: ") | dim,
            mode_dropdown->Render() | size(WIDTH, EQUAL, 16),
        }));
        children.push_back(text(mode_copy == kProofread
                                    ? "Each message is proofread on its own -- earlier ones are not context."
                                    : "A running conversation; earlier turns are sent along as context.") |
                           dim);
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(chat_pane->Render() | flex | reflect(pane_box_));

        if (sending_now) {
            // What the model is doing right now, in numbers: reasoning first,
            // then the answer streaming in.
            std::string detail;
            if (progress_copy.thinking_chars > 0)
                detail += std::to_string(progress_copy.thinking_chars) + " chars of reasoning";
            if (progress_copy.content_chars > 0) {
                if (!detail.empty()) detail += " · ";
                detail += std::to_string(progress_copy.content_chars) + " chars written";
            }
            const std::string verb = (mode_copy == kProofread && progress_copy.thinking_chars == 0)
                                         ? "Proofreading"
                                         : "";  // "" rotates through the waiting words
            children.push_back(ThinkingIndicator(started, verb, detail));
            if (progress_copy.reasoning && !progress_copy.thinking_tail.empty()) {
                children.push_back(text("  " + one_line(progress_copy.thinking_tail)) | dim |
                                   color(theme::kWarning));
            }
        }
        if (!status_copy.empty()) children.push_back(text(status_copy) | dim);

        children.push_back(hbox({
            input_box->Render() | flex | size(HEIGHT, LESS_THAN, 6) | borderStyled(ROUNDED, theme::kMuted),
            vbox({send_button->Render(), clear_button->Render()}),
        }));

        return vbox(children) | flex;
    });

    root_ |= CatchEvent([this](Event event) {
        if (!event.is_mouse()) return false;
        const auto& m = event.mouse();
        if (m.button != Mouse::Left || m.motion != Mouse::Pressed) return false;
        return handle_thinking_click(m.x, m.y);
    });
}

}  // namespace callscribe::ui
