#include "llm/ollama_client.hpp"

#include <curl/curl.h>

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

nlohmann::json generate_json(const std::string& host, const std::string& model, const std::string& prompt,
                              long timeout_seconds) {
    nlohmann::json req = {{"model", model}, {"prompt", prompt}, {"stream", false}, {"format", "json"}};
    std::string resp_body = http_post_json(host + "/api/generate", req.dump(), timeout_seconds);

    nlohmann::json outer;
    try {
        outer = nlohmann::json::parse(resp_body);
    } catch (const nlohmann::json::exception& e) {
        throw OllamaError(std::string("Ollama returned an invalid response envelope: ") + e.what());
    }
    std::string raw = outer.value("response", "");
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

std::string generate_text(const std::string& host, const std::string& model, const std::string& prompt,
                           long timeout_seconds) {
    nlohmann::json req = {{"model", model}, {"prompt", prompt}, {"stream", false}};
    std::string resp_body = http_post_json(host + "/api/generate", req.dump(), timeout_seconds);
    nlohmann::json outer;
    try {
        outer = nlohmann::json::parse(resp_body);
    } catch (const nlohmann::json::exception& e) {
        throw OllamaError(std::string("Ollama returned an invalid response envelope: ") + e.what());
    }
    return trim(outer.value("response", ""));
}

}  // namespace callscribe::llm
