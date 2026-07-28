#include "transcription/whisper_engine.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "config.hpp"

namespace callscribe::transcription {

bool cuda_build_available() {
    std::string info = whisper_print_system_info();
    return info.find("CUDA") != std::string::npos;
}

std::string suggest_model_size() { return cuda_build_available() ? "medium" : "small"; }

std::string model_filename(const std::string& size) { return "ggml-" + size + ".bin"; }

std::string model_download_url(const std::string& size) {
    return "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/" + model_filename(size);
}

void ensure_model_downloaded(const std::string& size) {
    std::filesystem::path path = models_dir() / model_filename(size);
    if (std::filesystem::exists(path)) return;

    std::filesystem::create_directories(models_dir());
    std::filesystem::path tmp_path = path;
    tmp_path += ".part";

    FILE* fp = std::fopen(tmp_path.string().c_str(), "wb");
    if (!fp) throw std::runtime_error("Failed to open " + tmp_path.string() + " for writing");

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string url = model_download_url(size);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (res != CURLE_OK || http_code >= 400) {
        std::filesystem::remove(tmp_path);
        std::string reason = res != CURLE_OK ? curl_easy_strerror(res) : ("HTTP " + std::to_string(http_code));
        throw std::runtime_error("Failed to download " + url + ": " + reason);
    }
    std::filesystem::rename(tmp_path, path);
}

WhisperEngine::WhisperEngine(const std::string& model_path) {
    if (!std::filesystem::exists(model_path)) {
        throw std::runtime_error("Whisper model not found: " + model_path +
                                  " (run scripts/setup_env.sh or download it from Settings)");
    }
    whisper_context_params cparams = whisper_context_default_params();
    ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    if (!ctx_) {
        throw std::runtime_error("Failed to load Whisper model: " + model_path);
    }
}

WhisperEngine::~WhisperEngine() {
    if (ctx_) whisper_free(ctx_);
}

std::string WhisperEngine::transcribe(const std::vector<float>& audio) {
    if (audio.empty() || !ctx_) return "";

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.translate = false;
    params.single_segment = false;
    params.n_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    params.language = detected_language_.empty() ? nullptr : detected_language_.c_str();

    if (whisper_full(ctx_, params, audio.data(), static_cast<int>(audio.size())) != 0) {
        throw std::runtime_error("whisper_full failed");
    }

    if (detected_language_.empty()) {
        int lang_id = whisper_full_lang_id(ctx_);
        if (lang_id >= 0) {
            const char* lang = whisper_lang_str(lang_id);
            if (lang) detected_language_ = lang;
        }
    }

    std::string text;
    int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx_, i);
        if (!seg) continue;
        std::string s(seg);
        size_t start = s.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        if (!text.empty()) text += " ";
        text += s.substr(start);
    }
    return text;
}

}  // namespace callscribe::transcription
