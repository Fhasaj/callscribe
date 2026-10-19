#include "llm/ollama_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>

#include "config.hpp"

namespace callscribe::llm {

namespace {

void ensure_curl_global_init() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlHandle {
    CURL* c;
    CurlHandle() : c(curl_easy_init()) {
        if (!c) throw OllamaError("curl_easy_init failed");
    }
    ~CurlHandle() {
        if (c) curl_easy_cleanup(c);
    }
    operator CURL*() const { return c; }
};

std::atomic<Backend>& backend_value() {
    static std::atomic<Backend> value{Backend::Ollama};
    return value;
}
// llama.cpp's context size in tokens, once read from /props; 0 = unknown.
std::atomic<long>& server_context_tokens() {
    static std::atomic<long> value{0};
    return value;
}

std::mutex& api_key_mutex() {
    static std::mutex m;
    return m;
}
std::string& api_key_value() {
    static std::string value;
    return value;
}

// The headers every request carries: JSON, and llama-server's key if it has
// one. Caller frees.
curl_slist* request_headers(bool json_body) {
    curl_slist* headers = json_body ? curl_slist_append(nullptr, "Content-Type: application/json") : nullptr;
    if (backend_value().load() == Backend::LlamaCpp) {
        std::lock_guard<std::mutex> lock(api_key_mutex());
        if (!api_key_value().empty())
            headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key_value()).c_str());
    }
    return headers;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url, long timeout_seconds) {
    ensure_curl_global_init();
    CurlHandle curl;
    std::string body;
    curl_slist* headers = request_headers(false);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) throw OllamaError(std::string("HTTP GET failed: ") + curl_easy_strerror(res));
    if (status == 401) throw OllamaError("the server wants an API key (or a different one)");
    return body;
}

std::string http_post_json(const std::string& url, const std::string& json_body, long timeout_seconds) {
    ensure_curl_global_init();
    CurlHandle curl;
    std::string body;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) throw OllamaError(std::string("HTTP POST failed: ") + curl_easy_strerror(res));
    return body;
}

// Guarded because Settings can save it from the UI thread while a summarize
// worker is mid-request on another.
std::mutex& keep_alive_mutex() {
    static std::mutex m;
    return m;
}
std::string& keep_alive_value() {
    static std::string value = kDefaultOllamaKeepAlive;
    return value;
}
std::string current_keep_alive() {
    std::lock_guard<std::mutex> lock(keep_alive_mutex());
    return keep_alive_value();
}

// Live ModelSessions per (host, model). Refcounted because a summarize worker
// and a chat reply can be in flight at once; only the last one out unloads.
std::mutex& session_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::pair<std::string, std::string>, int>& session_counts() {
    static std::map<std::pair<std::string, std::string>, int> counts;
    return counts;
}
std::atomic<bool>& unload_after_use_flag() {
    static std::atomic<bool> value{true};
    return value;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

struct ChatStreamContext {
    std::string buffer;
    std::string content;
    std::string thinking;
    std::string error;
    int prompt_eval_count = 0;
    bool done = false;
    const StreamCallback* on_progress = nullptr;
};

// One line of either stream format: Ollama's NDJSON (`{"message":{...}}`) or
// the OpenAI server-sent events llama.cpp sends (`data: {"choices":[{"delta":
// {...}}]}`, ending with `data: [DONE]`).
void consume_chat_line(ChatStreamContext* ctx, std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == ':') return;  // blank / SSE comment
    if (line.rfind("data:", 0) == 0) {
        line = trim(line.substr(5));
        if (line == "[DONE]") {
            ctx->done = true;
            return;
        }
    }
    try {
        nlohmann::json j = nlohmann::json::parse(line);
        if (j.contains("error")) {
            const auto& e = j.at("error");
            if (e.is_string()) ctx->error = e.get<std::string>();
            else if (e.is_object()) ctx->error = e.value("message", e.dump());
            return;
        }
        bool reasoning = false;
        if (j.contains("choices") && j.at("choices").is_array() && !j.at("choices").empty()) {
            const auto& choice = j.at("choices")[0];
            if (choice.contains("delta") && choice.at("delta").is_object()) {
                const auto& d = choice.at("delta");
                if (d.contains("content") && d.at("content").is_string())
                    ctx->content += d.at("content").get<std::string>();
                // llama-server splits reasoning out under this name when run
                // with --reasoning-format (the default for thinking models).
                if (d.contains("reasoning_content") && d.at("reasoning_content").is_string()) {
                    const std::string chunk = d.at("reasoning_content").get<std::string>();
                    ctx->thinking += chunk;
                    reasoning = !chunk.empty();
                }
            }
            if (choice.contains("finish_reason") && choice.at("finish_reason").is_string()) ctx->done = true;
        }
        if (j.contains("message")) {
            const auto& m = j.at("message");
            if (m.contains("content") && m.at("content").is_string())
                ctx->content += m.at("content").get<std::string>();
            if (m.contains("thinking") && m.at("thinking").is_string()) {
                const std::string chunk = m.at("thinking").get<std::string>();
                ctx->thinking += chunk;
                reasoning = !chunk.empty();
            }
        }
        if (j.contains("prompt_eval_count") && j.at("prompt_eval_count").is_number())
            ctx->prompt_eval_count = j.at("prompt_eval_count").get<int>();
        if (j.value("done", false)) ctx->done = true;

        if (ctx->on_progress && *ctx->on_progress) {
            StreamProgress p;
            p.content_chars = ctx->content.size();
            p.thinking_chars = ctx->thinking.size();
            p.reasoning = reasoning;
            constexpr size_t kTailChars = 160;
            p.thinking_tail = ctx->thinking.size() <= kTailChars
                                  ? ctx->thinking
                                  : ctx->thinking.substr(ctx->thinking.size() - kTailChars);
            (*ctx->on_progress)(p);
        }
    } catch (const nlohmann::json::exception&) {
        // Ignore a partial/malformed line and keep streaming.
    }
}

// Models whose template doesn't use Ollama's `thinking` field emit the same
// reasoning inline, fenced in <think>...</think> (qwen3, deepseek-r1 and
// friends). Lift it out so both kinds of model reach the UI the same way.
void extract_inline_thinking(std::string& content, std::string& thinking) {
    static const std::string kOpen = "<think>";
    static const std::string kClose = "</think>";
    size_t open = content.find(kOpen);
    while (open != std::string::npos) {
        const size_t body = open + kOpen.size();
        const size_t close = content.find(kClose, body);
        // An unterminated block means the model was cut off mid-reasoning:
        // take the rest as thinking rather than leaving the tag in the answer.
        const size_t end = (close == std::string::npos) ? content.size() : close;
        if (!thinking.empty()) thinking += "\n";
        thinking += content.substr(body, end - body);
        content.erase(open, (close == std::string::npos ? content.size() : close + kClose.size()) - open);
        open = content.find(kOpen, open);
    }
}

// Accumulates one streamed /api/chat chunk per line of NDJSON.
size_t chat_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ChatStreamContext*>(userdata);
    ctx->buffer.append(ptr, size * nmemb);
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        consume_chat_line(ctx, line);
    }
    return size * nmemb;
}

// Roughly how many tokens `text` is worth. English prose runs ~4 chars/token;
// 3 is deliberately pessimistic so we size the context window up rather than
// down -- undersizing means Ollama silently truncates the prompt.
long estimate_tokens(const std::string& text) {
    return static_cast<long>(text.size() / 3) + 1;
}

// Sends a single-turn chat request. Two things matter here:
//
// 1. /api/chat rather than /api/generate: harmony-style reasoning models
//    (gpt-oss) emit their analysis channel into `response` on /api/generate, so
//    the caller gets chain-of-thought instead of the answer. /api/chat applies
//    the chat template and splits reasoning out into `message.thinking`.
//
// 2. Streaming rather than a single blocking POST. Ollama sends nothing at all
//    while it evaluates a long prompt, so a non-streaming request looks like a
//    dead connection and dies on a fixed timeout partway through a long call.
//    Streaming keeps bytes flowing, which lets us drop the wall-clock deadline
//    entirely and fail only on a connection that has genuinely stalled.
ChatReply chat(const std::string& host, const std::string& model, const std::string& prompt, bool json_mode,
               long num_ctx, const StreamCallback* on_progress) {
    ensure_curl_global_init();
    CurlHandle curl;
    const bool llama = backend() == Backend::LlamaCpp;
    const char* server = llama ? "llama.cpp" : "Ollama";

    nlohmann::json req = {{"model", model},
                          {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})},
                          {"stream", true}};
    if (llama) {
        // The context size was fixed when llama-server started (-c); there is
        // nothing to send for it, and no keep_alive to speak of.
        if (json_mode) req["response_format"] = {{"type", "json_object"}};
    } else {
        if (json_mode) req["format"] = "json";
        // Sent per-request so it wins over the server's OLLAMA_KEEP_ALIVE: a
        // model parked in VRAM is memory Whisper can't have.
        if (std::string ka = current_keep_alive(); !ka.empty()) req["keep_alive"] = ka;
        // Without an explicit num_ctx, Ollama applies its own small default and
        // silently drops everything past it -- a long transcript comes back
        // summarized from the handful of tokens that survived, with no error.
        if (num_ctx > 0) req["options"] = {{"num_ctx", num_ctx}};
    }
    std::string body_str = req.dump();

    ChatStreamContext ctx;
    ctx.on_progress = on_progress;
    curl_slist* headers = request_headers(true);
    curl_easy_setopt(curl, CURLOPT_URL, (host + (llama ? "/v1/chat/completions" : "/api/chat")).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chat_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  // no wall-clock cap on generation
    // Prompt evaluation is silent, so allow a long quiet period before calling
    // the connection dead; after that, <1 byte/s for 15 min is a real stall.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 900L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 15L);

    CURLcode res = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);

    // Ollama's error replies are a bare {"error":...} object with no trailing
    // newline, so the streaming loop above never sees them as a line. Flush
    // whatever is left or the real reason is lost.
    consume_chat_line(&ctx, trim(ctx.buffer));

    if (res != CURLE_OK) {
        std::string msg = std::string("Lost the connection to ") + server + ": " + curl_easy_strerror(res);
        if (!ctx.error.empty()) msg += " (" + ctx.error + ")";
        throw OllamaError(msg);
    }
    if (!ctx.error.empty()) throw OllamaError(std::string(server) + " error: " + ctx.error);
    if (http_status >= 400)
        throw OllamaError(std::string(server) + " returned HTTP " + std::to_string(http_status) + ": " +
                          ctx.buffer.substr(0, 300));
    if (!ctx.done && trim(ctx.content).empty()) {
        throw OllamaError(std::string(server) +
                          " closed the stream without returning anything -- this usually means it ran out of "
                          "memory for the requested context size. Try a smaller model, or shorten the call.");
    }

    ChatReply reply;
    reply.content = ctx.content;
    reply.thinking = ctx.thinking;
    extract_inline_thinking(reply.content, reply.thinking);
    // A model that spends its whole budget reasoning can come back with empty
    // content; the thinking text is better than nothing to parse/show.
    if (trim(reply.content).empty()) reply.content = reply.thinking;
    return reply;
}

struct PullContext {
    std::string buffer;
    const std::function<void(const std::string&)>* on_progress;
    std::string error;
};

size_t pull_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<PullContext*>(userdata);
    ctx->buffer.append(ptr, size * nmemb);
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);
        if (line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            if (j.contains("error")) {
                ctx->error = j.at("error").get<std::string>();
                continue;
            }
            if (ctx->on_progress && *ctx->on_progress) {
                std::string status = j.value("status", "");
                if (j.contains("completed") && j.contains("total") && j.at("total").get<double>() > 0) {
                    double pct = 100.0 * j.at("completed").get<double>() / j.at("total").get<double>();
                    status += " (" + std::to_string(static_cast<int>(pct)) + "%)";
                }
                (*ctx->on_progress)(status);
            }
        } catch (const nlohmann::json::exception&) {
            // ignore malformed line, keep streaming
        }
    }
    return size * nmemb;
}

}  // namespace

void set_keep_alive(const std::string& value) {
    std::lock_guard<std::mutex> lock(keep_alive_mutex());
    keep_alive_value() = value;
}

void set_unload_after_use(bool value) { unload_after_use_flag().store(value); }

void set_api_key(const std::string& key) {
    std::lock_guard<std::mutex> lock(api_key_mutex());
    api_key_value() = key;
}

void set_backend(Backend value) {
    if (backend_value().exchange(value) != value) server_context_tokens().store(0);
}

Backend backend() { return backend_value().load(); }

long refresh_server_context(const std::string& host) {
    if (backend() != Backend::LlamaCpp) return 0;
    try {
        auto j = nlohmann::json::parse(http_get(host + "/props", 5));
        long n_ctx = 0;
        // Per-slot size: what one request actually gets when the server runs
        // several parallel slots.
        if (j.contains("default_generation_settings") && j["default_generation_settings"].is_object())
            n_ctx = j["default_generation_settings"].value("n_ctx", 0L);
        if (n_ctx <= 0) n_ctx = j.value("n_ctx", 0L);
        if (n_ctx > 0) server_context_tokens().store(n_ctx);
        return n_ctx;
    } catch (const std::exception&) {
        return 0;
    }
}

ModelSession::ModelSession(std::string host, std::string model)
    : host_(std::move(host)), model_(std::move(model)) {
    // llama-server keeps its one model for its whole life; there is no unload.
    if (model_.empty() || !unload_after_use_flag().load() || backend() == Backend::LlamaCpp) return;
    active_ = true;
    std::lock_guard<std::mutex> lock(session_mutex());
    ++session_counts()[{host_, model_}];
}

ModelSession::~ModelSession() {
    if (!active_) return;
    {
        std::lock_guard<std::mutex> lock(session_mutex());
        auto it = session_counts().find({host_, model_});
        if (it == session_counts().end()) return;
        if (--it->second > 0) return;  // another operation is still using it
        session_counts().erase(it);
    }
    // Outside the lock: this is an HTTP round trip, and holding the mutex
    // across it would stall any session starting on another thread.
    unload_model(host_, model_);
}

void unload_model(const std::string& host, const std::string& model) noexcept {
    if (model.empty() || backend() == Backend::LlamaCpp) return;
    // An empty prompt with keep_alive 0 is Ollama's documented way to evict a
    // model without generating anything.
    nlohmann::json req = {{"model", model}, {"prompt", ""}, {"keep_alive", 0}};
    try {
        http_post_json(host + "/api/generate", req.dump(), 10);
    } catch (const OllamaError&) {
        // Nothing useful to do -- the model stays loaded until it times out.
    }
}

bool is_running(const std::string& host) {
    try {
        http_get(host + (backend() == Backend::LlamaCpp ? "/health" : "/api/tags"), 3);
        return true;
    } catch (const OllamaError&) {
        return false;
    }
}

std::vector<std::string> list_models(const std::string& host) {
    if (backend() == Backend::LlamaCpp) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(http_get(host + "/v1/models", 10));
        } catch (const nlohmann::json::exception& e) {
            throw OllamaError(std::string("Could not parse the llama.cpp model list: ") + e.what());
        }
        std::vector<std::string> names;
        if (j.contains("data") && j.at("data").is_array()) {
            for (const auto& m : j.at("data")) names.push_back(m.value("id", ""));
        }
        refresh_server_context(host);
        return names;
    }
    std::string body = http_get(host + "/api/tags", 10);
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception& e) {
        throw OllamaError(std::string("Could not parse model list: ") + e.what());
    }
    std::vector<std::string> names;
    if (j.contains("models") && j.at("models").is_array()) {
        for (const auto& m : j.at("models")) names.push_back(m.value("name", ""));
    }
    return names;
}

void pull_model(const std::string& host, const std::string& model,
                 const std::function<void(const std::string&)>& on_progress) {
    if (backend() == Backend::LlamaCpp)
        throw OllamaError("llama.cpp loads its model when the server starts -- there is nothing to pull.");
    ensure_curl_global_init();
    CurlHandle curl;
    nlohmann::json req = {{"name", model}, {"stream", true}};
    std::string body_str = req.dump();

    PullContext ctx;
    ctx.on_progress = &on_progress;

    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, (host + "/api/pull").c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pull_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  // model pulls can take a while
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) throw OllamaError("Failed to pull " + model + ": " + curl_easy_strerror(res));
    if (!ctx.error.empty()) throw OllamaError("Failed to pull " + model + ": " + ctx.error);
}

// Below kMinContext there is no point shrinking. kMaxContext is the ceiling we
// will ask Ollama for: the KV cache is allocated for the whole window, so too
// large a value makes generation fail outright on a mid-range GPU (Ollama drops
// the connection rather than reporting anything useful). Anything longer than
// this gets chunked by the summarizer instead.
constexpr long kMinContext = 4096;
constexpr long kMaxContext = 32768;
constexpr long kOutputHeadroom = 2048;

long context_size_for(const std::string& prompt) {
    // Prompt tokens plus room for the answer, clamped to that range.
    long needed = estimate_tokens(prompt) + kOutputHeadroom;
    return std::clamp(needed, kMinContext, kMaxContext);
}

long max_prompt_chars() {
    // The largest prompt that fits the context at the estimate_tokens ratio,
    // used by the summarizer to decide when a transcript needs chunking. A
    // llama.cpp server's window is fixed and known, so use it -- bigger or
    // smaller than ours.
    long context = kMaxContext;
    if (backend() == Backend::LlamaCpp && server_context_tokens().load() > 0) context = server_context_tokens().load();
    return std::max(kMinContext, context - kOutputHeadroom) * 3;
}

nlohmann::json generate_json(const std::string& host, const std::string& model, const std::string& prompt) {
    std::string raw = chat(host, model, prompt, /*json_mode=*/true, context_size_for(prompt), nullptr).content;
    try {
        return nlohmann::json::parse(raw);
    } catch (const nlohmann::json::exception&) {
        size_t start = raw.find('{');
        size_t end = raw.rfind('}');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            try {
                return nlohmann::json::parse(raw.substr(start, end - start + 1));
            } catch (const nlohmann::json::exception&) {
                // fall through to error below
            }
        }
        throw OllamaError("Model did not return valid JSON: " + raw.substr(0, 300));
    }
}

std::string generate_text(const std::string& host, const std::string& model, const std::string& prompt) {
    return trim(chat(host, model, prompt, /*json_mode=*/false, context_size_for(prompt), nullptr).content);
}

ChatReply generate_reply(const std::string& host, const std::string& model, const std::string& prompt,
                          const StreamCallback& on_progress) {
    ChatReply reply = chat(host, model, prompt, /*json_mode=*/false, context_size_for(prompt),
                           on_progress ? &on_progress : nullptr);
    reply.content = trim(reply.content);
    reply.thinking = trim(reply.thinking);
    return reply;
}

}  // namespace callscribe::llm
