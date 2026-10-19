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

// Which server the LLM calls go to. Every function in this header takes a
// host and model either way; the backend decides what is said to them.
//
// LlamaCpp is llama.cpp's `llama-server`, spoken to through its
// OpenAI-compatible /v1/chat/completions. It loads one model when it starts and
// keeps it, so there is nothing to pull or unload and keep_alive does not apply;
// the context window is whatever -c the server was started with, read from
// /props (see refresh_server_context).
enum class Backend { Ollama, LlamaCpp };
void set_backend(Backend backend);
Backend backend();
// llama-server's --api-key, sent as a Bearer token. Empty = none. Never sent
// to Ollama, which has no such thing.
void set_api_key(const std::string& key);

// Asks a llama.cpp server for its context size so prompts are budgeted to fit
// it. Best-effort: on failure the Ollama-sized default stays in force. Returns
// the size in tokens, or 0 if it couldn't be read.
long refresh_server_context(const std::string& host);

// Sets the `keep_alive` sent with every subsequent chat request -- how long
// Ollama holds the model in (V)RAM once the reply is done. Set once at startup
// and again whenever Settings is saved, rather than being threaded through
// every call, since it is a single process-wide preference. Accepts Ollama's
// duration format ("10m", "1h", "0", "-1"); an empty value omits the field and
// lets the server's OLLAMA_KEEP_ALIVE decide.
void set_keep_alive(const std::string& value);

// Whether a model is evicted from memory as soon as the operation that needed
// it is done (see ModelSession). Set from Settings the same way keep_alive is.
void set_unload_after_use(bool value);

// Keeps a model resident for the span of one logical operation, then hands the
// memory back.
//
// Summarizing is not one request but a chain of them -- a dozen map calls over
// a long transcript, then the reduce -- so evicting after each reply would pay
// the load cost over and over. Instead the whole operation is one session:
// keep_alive bridges the gaps inside it, and the last session to close asks
// Ollama to drop the model immediately.
//
// This matters because callscribe is not the only thing on the machine wanting
// VRAM: Whisper needs room to transcribe, and any other Ollama client (an
// editor, an agent) is loading its own weights alongside ours. A summary model
// parked for keep_alive minutes after the work is done is memory nobody can
// use. Sessions nest and refcount, so overlapping operations on the same model
// only unload once, when the last one finishes.
class ModelSession {
public:
    ModelSession(std::string host, std::string model);
    ~ModelSession();
    ModelSession(const ModelSession&) = delete;
    ModelSession& operator=(const ModelSession&) = delete;

private:
    std::string host_;
    std::string model_;
    bool active_ = false;
};

bool is_running(const std::string& host);
// Ollama: everything installed. llama.cpp: the model(s) the server has loaded.
std::vector<std::string> list_models(const std::string& host);

// Asks Ollama to drop `model` from memory right now (keep_alive: 0), freeing
// the VRAM for Whisper. Best-effort: a failure here is not worth surfacing.
void unload_model(const std::string& host, const std::string& model) noexcept;
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
