#include "transcription/whisper_engine.hpp"

#include <curl/curl.h>
#include <ggml-backend.h>

#include <algorithm>
#include <system_error>
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

namespace {

std::string format_mib(size_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

struct GpuChoice {
    bool any_gpu = false;   // a GPU backend exists at all
    int device = -1;        // ordinal with room, or -1 if none
    size_t best_free = 0;   // most free VRAM seen, for the message
};

// Weights are only part of it -- the KV cache, compute buffers and allocator
// fragmentation all need room on top, so require a healthy margin over the
// file size rather than just fitting the weights. The weights are loaded once
// and shared, but every concurrent state carries its own working buffers, so
// that margin is charged per state.
//
// First fit wins deliberately: CUDA enumerates fastest-first by default, so the
// first device that has room is the best card that has room, and we only drop
// to a slower one when the faster is full. (Note this ordering is the reverse
// of nvidia-smi's, which numbers by PCI bus -- on a mixed pair, CUDA0 is not
// necessarily nvidia-smi's GPU 0.) Picking the device with the most free memory
// instead would sometimes land on the slower card.
GpuChoice choose_gpu(size_t model_bytes, int state_count) {
    GpuChoice choice;
    const size_t per_state = model_bytes / 3 + (size_t{256} << 20);
    const size_t needed = model_bytes + per_state * static_cast<size_t>(std::max(1, state_count));
    int ordinal = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_GPU) continue;
        choice.any_gpu = true;
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total_bytes);
        choice.best_free = std::max(choice.best_free, free_bytes);
        if (free_bytes >= needed) {
            choice.device = ordinal;
            return choice;
        }
        ++ordinal;
    }
    return choice;
}

}  // namespace

WhisperModel::WhisperModel(const std::string& model_path, int expected_states) {
    if (!std::filesystem::exists(model_path)) {
        throw std::runtime_error("Whisper model not found: " + model_path +
                                  " (run scripts/setup_env.sh or download it from Settings)");
    }
    whisper_context_params cparams = whisper_context_default_params();

    std::error_code ec;
    size_t model_bytes = static_cast<size_t>(std::filesystem::file_size(model_path, ec));
    if (!ec && cparams.use_gpu) {
        GpuChoice choice = choose_gpu(model_bytes, expected_states);
        if (choice.any_gpu && choice.device < 0) {
            // Loading anyway would abort inside ggml and kill the process.
            cparams.use_gpu = false;
            backend_note_ = "Not enough free VRAM for the Whisper model (" + format_mib(model_bytes) +
                            " model, " + format_mib(choice.best_free) +
                            " free) -- transcribing on CPU, which is slower. Free some GPU memory "
                            "(Settings > Unload now, or a shorter keep-alive) and reload to use the GPU.";
        } else if (choice.device >= 0) {
            cparams.gpu_device = choice.device;
        }
    }

    // _no_state: the states are created per stream by WhisperEngine below.
    ctx_ = whisper_init_from_file_with_params_no_state(model_path.c_str(), cparams);
    if (!ctx_) {
        throw std::runtime_error("Failed to load Whisper model: " + model_path);
    }
}

WhisperModel::~WhisperModel() {
    if (ctx_) whisper_free(ctx_);
}

WhisperEngine::WhisperEngine(std::shared_ptr<WhisperModel> model) : model_(std::move(model)) {
    if (!model_ || !model_->raw()) throw std::runtime_error("Whisper model not loaded");
    state_ = whisper_init_state(model_->raw());
    if (!state_) throw std::runtime_error("Failed to allocate a Whisper inference state");
}

WhisperEngine::~WhisperEngine() {
    if (state_) whisper_free_state(state_);
}

std::string WhisperEngine::transcribe(const std::vector<float>& audio) {
    if (audio.empty() || !state_) return "";

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.translate = false;
    params.single_segment = false;
    params.n_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    params.language = detected_language_.empty() ? nullptr : detected_language_.c_str();

    if (whisper_full_with_state(model_->raw(), state_, params, audio.data(), static_cast<int>(audio.size())) != 0) {
        throw std::runtime_error("whisper_full failed");
    }

    if (detected_language_.empty()) {
        int lang_id = whisper_full_lang_id_from_state(state_);
        if (lang_id >= 0) {
            const char* lang = whisper_lang_str(lang_id);
            if (lang) detected_language_ = lang;
        }
    }

    std::string text;
    int n_segments = whisper_full_n_segments_from_state(state_);
    for (int i = 0; i < n_segments; ++i) {
        const char* seg = whisper_full_get_segment_text_from_state(state_, i);
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
