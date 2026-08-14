#include "llm/ollama_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <mutex>

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

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string http_get(const std::string& url, long timeout_seconds) {
    ensure_curl_global_init();
    CurlHandle curl;
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) throw OllamaError(std::string("HTTP GET failed: ") + curl_easy_strerror(res));
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

void consume_chat_line(ChatStreamContext* ctx, const std::string& line) {
    if (line.empty()) return;
    try {
        nlohmann::json j = nlohmann::json::parse(line);
        if (j.contains("error") && j.at("error").is_string()) {
            ctx->error = j.at("error").get<std::string>();
            return;
        }
        bool reasoning = false;
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

    nlohmann::json req = {{"model", model},
                          {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})},
                          {"stream", true}};
    if (json_mode) req["format"] = "json";
    // Without an explicit num_ctx, Ollama applies its own small default and
    // silently drops everything past it -- a long transcript comes back
    // summarized from the handful of tokens that survived, with no error.
    if (num_ctx > 0) req["options"] = {{"num_ctx", num_ctx}};
    std::string body_str = req.dump();

    ChatStreamContext ctx;
    ctx.on_progress = on_progress;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, (host + "/api/chat").c_str());
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
        std::string msg = std::string("Lost the connection to Ollama: ") + curl_easy_strerror(res);
        if (!ctx.error.empty()) msg += " (" + ctx.error + ")";
        throw OllamaError(msg);
    }
    if (!ctx.error.empty()) throw OllamaError("Ollama error: " + ctx.error);
    if (http_status >= 400)
        throw OllamaError("Ollama returned HTTP " + std::to_string(http_status) + ": " + ctx.buffer.substr(0, 300));
    if (!ctx.done && trim(ctx.content).empty()) {
        throw OllamaError(
            "Ollama closed the stream without returning anything -- this usually means it ran out of "
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

bool is_running(const std::string& host) {
    try {
        http_get(host + "/api/tags", 3);
        return true;
    } catch (const OllamaError&) {
        return false;
    }
}

std::vector<std::string> list_models(const std::string& host) {
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
    // The largest prompt that fits kMaxContext at the estimate_tokens ratio,
    // used by the summarizer to decide when a transcript needs chunking.
    return (kMaxContext - kOutputHeadroom) * 3;
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
