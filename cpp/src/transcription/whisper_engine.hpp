#pragma once
#include <whisper.h>

#include <memory>
#include <string>
#include <vector>

#include "util/download.hpp"

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

// A run of speech that whisper attributed to one speaker. `turn_after` is set
// on the last utterance before tinydiarize saw the speaker change -- so a chunk
// with two people talking comes back as two Utterances, the first flagged.
// Without tinydiarize every chunk is a single unflagged Utterance.
struct Utterance {
    std::string text;
    bool turn_after = false;
    // Milliseconds from the start of the chunk that was passed in. The caller
    // adds the chunk's own offset to place these in the recording -- which is
    // what lets diarization's speaker time ranges be matched to this text.
    int64_t start_ms = 0;
    int64_t end_ms = 0;
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

    // Same inference, but grouped into speaker turns. `detect_turns` only has
    // an effect on a tinydiarize model (see model_supports_speaker_turns);
    // anywhere else it costs nothing and the result is one Utterance.
    std::vector<Utterance> transcribe_turns(const std::vector<float>& audio, bool detect_turns);

    // Frees this stream's KV cache and compute buffers, keeping the shared
    // weights loaded. The state is re-created on the next transcribe(), so
    // this is safe to call whenever a recording ends -- which is the point:
    // between calls the app is idle, and on `medium` those per-stream buffers
    // are several hundred MB of GPU (or host) memory sitting unused.
    void release_state();

    const std::string& backend_note() const { return model_->backend_note(); }

private:
    std::shared_ptr<WhisperModel> model_;
    whisper_state* state_ = nullptr;
    std::string detected_language_;

    // Allocates state_ on first use. Throws if whisper won't give us one.
    whisper_state* ensure_state();
};

// True if whisper.cpp/ggml was compiled with a CUDA backend available.
bool cuda_build_available();
std::string suggest_model_size();

// True for the tinydiarize models, the only ones whisper.cpp can detect
// speaker turns with. Note what this does and does not give you: tinydiarize
// marks that the speaker *changed*, never who is talking -- and it is trained
// on English only, at `small` quality. Everything else silently ignores the
// flag, which is why the Settings toggle is gated on this.
bool model_supports_speaker_turns(const std::string& size);
std::string model_filename(const std::string& size);      // "ggml-small.bin"
std::string model_download_url(const std::string& size);  // HF mirror URL

// True when the weights for `size` are already on disk.
bool model_present(const std::string& size);

// Downloads the GGML model file for `size` into the models dir if it's not
// already there. Blocking -- call from a worker thread. Throws
// std::runtime_error on failure. `on_progress` is optional and is called from
// this thread while the transfer runs.
void ensure_model_downloaded(const std::string& size, const util::ProgressCallback& on_progress = nullptr);

}  // namespace callscribe::transcription
