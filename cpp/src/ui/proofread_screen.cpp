#include "ui/proofread_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include "llm/summarizer.hpp"

namespace callscribe::ui {

ProofreadScreen::ProofreadScreen(AppContext& ctx) : ctx_(ctx) { build_ui(); }

ProofreadScreen::~ProofreadScreen() {
    if (worker_.joinable()) worker_.join();
}

void ProofreadScreen::send() {
    std::string email = input_text_;
    size_t start = email.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;  // nothing but whitespace

    std::string model = ctx_.settings.ollama_model;
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "No Ollama model selected -- pick one in Settings.";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_.push_back({true, email});
        status_message_ = "Proofreading...";
        sending_ = true;
    }
    input_text_.clear();

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, email, model] {
        std::string result_text, error_text;
        try {
            result_text = llm::proofread_email(ctx_.settings.ollama_host, model, email);
        } catch (const std::exception& e) {
            error_text = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_text.empty()) {
            conversation_.push_back({false, result_text});
            status_message_.clear();
        } else {
            status_message_ = "Failed: " + error_text;
        }
        sending_ = false;
    });
}

void ProofreadScreen::build_ui() {
    using namespace ftxui;

    InputOption input_opt;
    input_opt.multiline = true;
    // Deliberately no on_enter -> send binding: multiline Input inserts a
    // newline AND fires on_enter on every Return, which would send
    // mid-paste/mid-compose. Submission is via the explicit Send button.
    auto input_box = Input(&input_text_, "Paste or type an email to proofread...", input_opt);
    auto send_button = Button("Send", [this] { send(); });

    auto layout = Container::Vertical({input_box, send_button});

    root_ = Renderer(layout, [this, input_box, send_button] {
        std::vector<Message> conv_copy;
        std::string status_copy;
        bool sending_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            conv_copy = conversation_;
            status_copy = status_message_;
            sending_now = sending_;
        }

        Elements bubbles;
        for (size_t i = 0; i < conv_copy.size(); ++i) {
            const auto& msg = conv_copy[i];
            Element bubble_body = vbox({
                                       text(msg.is_user ? "You" : "CallScribe") | bold |
                                           color(msg.is_user ? Color(Color::Cyan) : Color(Color::Green)),
                                       paragraph(msg.text) | border,
                                   }) |
                                   size(WIDTH, LESS_THAN, 76);
            Element row = msg.is_user ? hbox({filler(), bubble_body}) : hbox({bubble_body, filler()});
            if (i + 1 == conv_copy.size()) row = focus(row);
            bubbles.push_back(row);
        }
        if (bubbles.empty()) {
            bubbles.push_back(text("Paste an email below and press Send for a proofread, corrected version.") |
                               dim | center);
        }

        Elements children;
        children.push_back(text("CallScribe -- Proofread") | bold);
        children.push_back(separator());
        children.push_back(yframe(vbox(bubbles)) | flex | border);
        if (sending_now) children.push_back(text("Proofreading...") | color(Color(Color::Yellow)));
        if (!status_copy.empty()) children.push_back(text(status_copy) | dim);
        children.push_back(hbox({
            input_box->Render() | flex | size(HEIGHT, LESS_THAN, 6) | border,
            send_button->Render(),
        }));

        return vbox(children) | flex;
    });
}

}  // namespace callscribe::ui
