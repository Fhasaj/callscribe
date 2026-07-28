#pragma once
#include <whisper.h>

#include <string>
#include <vector>

namespace callscribe::transcription {

// Wraps a whisper.cpp context. whisper.cpp contexts are not safe for
// concurrent whisper_full() calls from multiple threads on the SAME
// instance, so the app creates one WhisperEngine per audio stream (mic,
// monitor) -- each stream's worker thread owns its instance. GPU vs CPU
// is decided at build time (ggml picks the best compiled-in backend), not
// per-instance here.
class WhisperEngine {
public:
    explicit WhisperEngine(const std::string& model_path);
    ~WhisperEngine();
    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // audio must be mono float32 @ 16kHz in [-1, 1].
    std::string transcribe(const std::vector<float>& audio);

private:
    whisper_context* ctx_ = nullptr;
    std::string detected_language_;
};

// True if whisper.cpp/ggml was compiled with a CUDA backend available.
bool cuda_build_available();
std::string suggest_model_size();
std::string model_filename(const std::string& size);      // "ggml-small.bin"
std::string model_download_url(const std::string& size);  // HF mirror URL

// Downloads the GGML model file for `size` into the models dir if it's not
// already there. Blocking -- call from a worker thread. Throws
// std::runtime_error on failure.
void ensure_model_downloaded(const std::string& size);

}  // namespace callscribe::transcription
