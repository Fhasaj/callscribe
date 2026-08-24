#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "llm/ollama_client.hpp"

namespace callscribe::ui {

// Chat-style screen against the local Ollama model, in one of two modes:
//
//   Chat      -- a free-form assistant that remembers the thread.
//   Proofread -- paste an email, get a corrected version back; each message is
//                independent, nothing carries over between them.
//
// Each mode keeps its own transcript, so switching the dropdown swaps the
// conversation rather than interleaving two unrelated ones.
//
// Reasoning models return their chain of thought separately from the answer;
// it is folded away behind a per-message toggle rather than shown inline,
// since it is usually long and rarely what you came for.
class ChatScreen {
public:
    explicit ChatScreen(AppContext& ctx);
    ~ChatScreen();
    ChatScreen(const ChatScreen&) = delete;
    ChatScreen& operator=(const ChatScreen&) = delete;

    ftxui::Component component() { return root_; }
    // True while a reply is streaming in, so the render loop keeps ticking
    // fast enough for the progress counters to move.
    bool needs_fast_redraw() const { return streaming_.load(); }

private:
    struct Message {
        bool is_user = false;
        std::string text;
        std::string thinking;   // empty unless the model reasons
        bool thinking_open = false;
    };

    enum Mode : int { kChat = 0, kProofread = 1, kModeCount = 2 };

    AppContext& ctx_;
    std::string input_text_;
    std::vector<std::string> mode_labels_{"Chat", "Proofread"};
    int mode_ = kChat;

    std::mutex mutex_;
    // Indexed by Mode -- see the class comment.
    std::vector<Message> conversations_[kModeCount];
    std::string status_message_;
    bool sending_ = false;
    // Mirrors sending_ for the render-loop ticker, which polls it every tick
    // from another thread and shouldn't have to take mutex_ to do so.
    std::atomic<bool> streaming_{false};
    std::chrono::steady_clock::time_point send_started_;
    llm::StreamProgress progress_;
    std::thread worker_;

    // Screen rectangles of the "thinking" toggle rows, refreshed on every
    // render and hit-tested on click. Parallel to conversations_[mode_];
    // entries for messages without reasoning stay empty.
    std::vector<ftxui::Box> thinking_boxes_;
    ftxui::Box pane_box_;

    ftxui::Component root_;

    void send();
    void clear_conversation();
    bool handle_thinking_click(int x, int y);
    void build_ui();
};

}  // namespace callscribe::ui
