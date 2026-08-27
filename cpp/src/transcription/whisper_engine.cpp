#include "transcription/whisper_engine.hpp"

#include <ggml-backend.h>

#include <algorithm>
#include <cmath>
#include <system_error>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

#include "config.hpp"
#include "util/download.hpp"

namespace callscribe::transcription {

bool cuda_build_available() {
    std::string info = whisper_print_system_info();
    return info.find("CUDA") != std::string::npos;
}

std::string suggest_model_size() { return cuda_build_available() ? "medium" : "small"; }

bool model_supports_speaker_turns(const std::string& size) {
    return size.find("tdrz") != std::string::npos;
}

std::string model_filename(const std::string& size) { return "ggml-" + size + ".bin"; }

std::string model_download_url(const std::string& size) {
    // The tinydiarize models are not in whisper.cpp's own model repo -- they
    // come from the fork that trained them, which is what upstream's
    // models/download-ggml-model.sh does too.
    const char* repo = model_supports_speaker_turns(size) ? "akashmjn/tinydiarize-whisper.cpp"
                                                           : "ggerganov/whisper.cpp";
    return std::string("https://huggingface.co/") + repo + "/resolve/main/" + model_filename(size);
}

bool model_present(const std::string& size) {
    return std::filesystem::exists(models_dir() / model_filename(size));
}

void ensure_model_downloaded(const std::string& size, const util::ProgressCallback& on_progress) {
    std::filesystem::path path = models_dir() / model_filename(size);
    if (std::filesystem::exists(path)) return;
    util::download_to_file(model_download_url(size), path, on_progress);
}

namespace {

std::string format_mib(size_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MiB"; }

// Loudest sample a chunk must reach before it is worth transcribing at all.
//
// Whisper invents speech when handed silence -- "Thank you.", "Thanks for
// watching!" and friends are in its training data at the points where nothing
// is being said, and it will emit them confidently for a chunk of pure room
// tone. `small.en` is among the worst for it, so turning on tinydiarize makes
// the problem very visible.
//
// Peak, deliberately, not RMS: a 12-second chunk holding one short sentence
// averages out to near-silence, so an RMS gate would throw away exactly the
// quiet-but-real speech we care about. Peak only asks "did anything in here
// ever get near speaking volume", and genuine speech clears 0.02 (-34 dBFS)
// with room to spare even from a distant or quiet talker.
constexpr float kSilencePeak = 0.02f;

bool is_silence(const std::vector<float>& audio) {
    for (float sample : audio)
        if (std::fabs(sample) >= kSilencePeak) return false;
    return true;
}

// Whisper's own estimate that a segment is not speech. It already uses this
// internally for timestamp decisions but still hands the segment over, so the
// filtering has to happen here. 0.6 is whisper's own default threshold.
constexpr float kNoSpeechThreshold = 0.6f;

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
    // The state is NOT allocated here. It is the expensive part per stream --
    // KV cache plus compute buffers -- and the app spends most of its life not
    // recording, so it is created on the first chunk and released again by
    // release_state() when the call ends.
}

WhisperEngine::~WhisperEngine() { release_state(); }

whisper_state* WhisperEngine::ensure_state() {
    if (!state_) {
        state_ = whisper_init_state(model_->raw());
        if (!state_) throw std::runtime_error("Failed to allocate a Whisper inference state");
    }
    return state_;
}

void WhisperEngine::release_state() {
    if (!state_) return;
    whisper_free_state(state_);
    state_ = nullptr;
}

std::string WhisperEngine::transcribe(const std::vector<float>& audio) {
    std::string text;
    for (const Utterance& u : transcribe_turns(audio, /*detect_turns=*/false)) {
        if (u.text.empty()) continue;
        if (!text.empty()) text += " ";
        text += u.text;
    }
    return text;
}

std::vector<Utterance> WhisperEngine::transcribe_turns(const std::vector<float>& audio, bool detect_turns) {
    // Cheapest and most effective of the three anti-hallucination measures
    // here, and it saves the inference entirely -- most chunks of a call are
    // silence on one side or the other.
    if (audio.empty() || is_silence(audio)) return {};
    whisper_state* state = ensure_state();

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_special = false;
    params.print_timestamps = false;
    params.translate = false;
    params.single_segment = false;
    // Chunks are handed over already split on ~12s boundaries and each one is
    // transcribed independently, so carrying the previous chunk's text in as a
    // prompt buys little -- and it grows the decoder's context (and the
    // memory behind it) chunk after chunk over a long call.
    params.no_context = true;
    // Two engines transcribe concurrently (mic and far end), so a full
    // hardware_concurrency each oversubscribes the machine twofold and doubles
    // the per-thread working buffers. Half each, at least one.
    params.n_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency() / 2));
    params.language = detected_language_.empty() ? nullptr : detected_language_.c_str();
    params.tdrz_enable = detect_turns;
    // Non-speech tokens (music cues, "[BLANK_AUDIO]", punctuation-only output)
    // are suppressed rather than transcribed, and the temperature fallback is
    // off: on a near-silent chunk the retry loop is what escalates a shaky
    // decode into a confident invented sentence. Same choice whisper.cpp's own
    // streaming example makes.
    params.suppress_nst = true;
    params.no_speech_thold = kNoSpeechThreshold;
    params.temperature_inc = 0.0f;

    if (whisper_full_with_state(model_->raw(), state, params, audio.data(), static_cast<int>(audio.size())) != 0) {
        throw std::runtime_error("whisper_full failed");
    }

    if (detected_language_.empty()) {
        int lang_id = whisper_full_lang_id_from_state(state);
        if (lang_id >= 0) {
            const char* lang = whisper_lang_str(lang_id);
            if (lang) detected_language_ = lang;
        }
    }

    // Segments are glued together until whisper flags a speaker change, so an
    // utterance is a whole turn rather than one 5-second fragment of one.
    std::vector<Utterance> out;
    Utterance current;
    bool current_started = false;
    int n_segments = whisper_full_n_segments_from_state(state);
    for (int i = 0; i < n_segments; ++i) {
        // Last line of defence: whisper scored this segment as probably not
        // speech, but produced text for it anyway.
        if (whisper_full_get_segment_no_speech_prob_from_state(state, i) > kNoSpeechThreshold) continue;
        const char* seg = whisper_full_get_segment_text_from_state(state, i);
        if (!seg) continue;
        std::string s(seg);
        size_t start = s.find_first_not_of(' ');
        if (start != std::string::npos) {
            if (!current.text.empty()) current.text += " ";
            current.text += s.substr(start);
            // whisper reports segment times in centiseconds. An utterance spans
            // from the first segment folded into it to the last.
            if (!current_started) {
                current.start_ms = whisper_full_get_segment_t0_from_state(state, i) * 10;
                current_started = true;
            }
            current.end_ms = whisper_full_get_segment_t1_from_state(state, i) * 10;
        }
        if (detect_turns && whisper_full_get_segment_speaker_turn_next_from_state(state, i)) {
            if (!current.text.empty()) {
                current.turn_after = true;
                out.push_back(std::move(current));
            }
            current = Utterance{};
            current_started = false;
        }
    }
    if (!current.text.empty()) out.push_back(std::move(current));
    return out;
}

}  // namespace callscribe::transcription
