#pragma once
#include <whisper.h>

#include <memory>
#include <string>
#include <vector>

namespace callscribe::transcription {

// The model weights, loaded once and shared by every stream.
//
// whisper.cpp separates the read-only context (weights) from the per-inference
// state (KV cache, buffers). Concurrent whisper_full() calls are unsafe on one
// *state*, not on one context -- so each audio stream gets its own state while
// they all share these weights. Loading the file once per stream instead would
// cost a second full copy of the model (~1.5 GB on `medium`).
//
// GPU or CPU is chosen when this is constructed, based on the VRAM actually
// free at that moment: ggml calls abort() if a device allocation fails, which
// would take the whole app down with a core dump and no message, so we check
// before asking rather than recovering after.
class WhisperModel {
public:
    // `expected_states` is how many concurrent streams will run off this model;
    // each one needs its own working buffers on top of the shared weights, so
    // the VRAM check has to account for all of them up front.
    WhisperModel(const std::string& model_path, int expected_states);
    ~WhisperModel();
    WhisperModel(const WhisperModel&) = delete;
    WhisperModel& operator=(const WhisperModel&) = delete;

    whisper_context* raw() const { return ctx_; }

    // Non-empty when the model had to fall back to CPU, explaining why.
    const std::string& backend_note() const { return backend_note_; }

private:
    whisper_context* ctx_ = nullptr;
    std::string backend_note_;
};

// One stream's inference state over a shared WhisperModel. Each instance is
// owned by exactly one worker thread; two instances may run concurrently.
class WhisperEngine {
public:
    explicit WhisperEngine(std::shared_ptr<WhisperModel> model);
    ~WhisperEngine();
    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // audio must be mono float32 @ 16kHz in [-1, 1].
    std::string transcribe(const std::vector<float>& audio);

    const std::string& backend_note() const { return model_->backend_note(); }

private:
    std::shared_ptr<WhisperModel> model_;
    whisper_state* state_ = nullptr;
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
