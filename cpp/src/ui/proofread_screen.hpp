#pragma once
#include <ftxui/component/component.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"

namespace callscribe::ui {

// Chat-style screen: paste/type an email, "Send" runs it through the local
// Ollama model for proofreading, and the corrected version comes back as
// a reply bubble -- a running conversation, not a static two-pane form.
class ProofreadScreen {
public:
    explicit ProofreadScreen(AppContext& ctx);
    ~ProofreadScreen();
    ProofreadScreen(const ProofreadScreen&) = delete;
    ProofreadScreen& operator=(const ProofreadScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    struct Message {
        bool is_user;
        std::string text;
    };

    AppContext& ctx_;
    std::string input_text_;

    std::mutex mutex_;
    std::vector<Message> conversation_;
    std::string status_message_;
    bool sending_ = false;
    std::thread worker_;

    ftxui::Component root_;

    void send();
    void build_ui();
};

}  // namespace callscribe::ui
