#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace callscribe::llm {

class OllamaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

bool is_running(const std::string& host);
std::vector<std::string> list_models(const std::string& host);
void pull_model(const std::string& host, const std::string& model,
                 const std::function<void(const std::string&)>& on_progress = nullptr);

// Runs a JSON-mode generate call and returns the parsed inner object
// (falls back to extracting the outermost {...} if the model wraps its
// JSON in stray text).
//
// Both calls stream internally and size the model's context window to the
// prompt, so there is no wall-clock deadline -- a long call takes as long as it
// takes, and only a genuinely stalled connection is an error.
nlohmann::json generate_json(const std::string& host, const std::string& model, const std::string& prompt);
std::string generate_text(const std::string& host, const std::string& model, const std::string& prompt);

// A reply with the model's reasoning kept alongside the answer instead of
// discarded. `thinking` is empty for models that don't reason; it is collected
// both from Ollama's `message.thinking` field and from an inline
// <think>...</think> block, since which one a model uses depends on its
// template rather than on anything we control.
struct ChatReply {
    std::string content;
    std::string thinking;
};

// Reported while a reply streams in, so the UI can show progress rather than a
// frozen screen. Called from the requesting thread, once per received chunk.
struct StreamProgress {
    std::size_t content_chars = 0;
    std::size_t thinking_chars = 0;
    bool reasoning = false;       // this chunk was reasoning, not answer text
    std::string thinking_tail;    // last ~160 chars of reasoning, for a live preview
};
using StreamCallback = std::function<void(const StreamProgress&)>;

ChatReply generate_reply(const std::string& host, const std::string& model, const std::string& prompt,
                          const StreamCallback& on_progress = nullptr);

// Largest prompt (in characters) that still fits the biggest context window we
// are willing to ask for. Callers with more input than this must split it.
long max_prompt_chars();

}  // namespace callscribe::llm
