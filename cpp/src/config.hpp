#pragma once
#include <filesystem>
#include <string>

namespace callscribe {

inline const char* kOllamaHostDefault = "http://localhost:11434";
inline const char* kRecommendedOllamaModel = "qwen2.5:7b-instruct";
// How long Ollama keeps the model resident after a call finishes. Sent with
// every request, so it overrides whatever OLLAMA_KEEP_ALIVE the server was
// started with. This is deliberately short: a model left loaded holds onto the
// VRAM that WhisperEngine needs, and Whisper silently falls back to (much
// slower) CPU transcription when it can't find room. Ollama's own format --
// "10m", "1h", "0" to unload immediately, "-1" to keep forever.
inline const char* kDefaultOllamaKeepAlive = "10m";
inline const char* kDefaultUserDisplayName = "Fatlum Hasaj";
inline const char* kDefaultOtherSpeakerLabel = "Other";

// User-editable instructions for what the model should produce. The JSON
// envelope around them is fixed (the app has to parse the reply), but the
// wording of each part is a preset the user can rewrite in Settings.
inline const char* kDefaultSummaryPrompt =
    "Summarize this meeting as short bullet points capturing the key points discussed.";
inline const char* kDefaultEmailPrompt =
    "Write a professional follow-up email for the customer with clear points and the agreed tasks, "
    "ready to send, signed off generically (e.g. \"Best regards\").";
inline const char* kDefaultTasksPrompt =
    "List the concrete action items mentioned or implied in the call, one entry per action.";

std::filesystem::path config_dir();
std::filesystem::path config_file();
std::filesystem::path data_dir();
std::filesystem::path recordings_dir();
std::filesystem::path models_dir();
std::filesystem::path default_db_path();

struct Settings {
    std::string db_path;
    std::string ollama_host = kOllamaHostDefault;
    std::string ollama_model;          // empty = not chosen yet, pick in Settings screen
    std::string ollama_keep_alive = kDefaultOllamaKeepAlive;
    // Evict the model the moment the summary/chat reply is done, instead of
    // letting it sit out its keep_alive. On by default: most machines running
    // this also run Whisper, and often another Ollama client of their own, and
    // paying a reload per operation is cheaper than a model parked in VRAM.
    // Turn it off on a machine with memory to spare and back-to-back use.
    bool ollama_unload_after_use = true;
    std::string whisper_model_size;    // empty = auto-suggest ("small" CPU / "medium" CUDA)
    // Split the far end of the call into speaker turns using whisper.cpp's
    // tinydiarize. Only the tdrz models can do this (see
    // transcription::model_supports_speaker_turns), and it detects that the
    // speaker *changed*, never who is speaking -- the participant list supplies
    // the names, in the order they first speak. Off by default: turning it on
    // means giving up the multilingual, higher-quality models for an
    // English-only `small`.
    bool detect_speaker_turns = false;
    // Run offline speaker diarization over the far-end audio once a call ends,
    // and rewrite the transcript's speaker labels with the result. Unlike the
    // tinydiarize option above this works with any Whisper model, because it
    // is a separate pair of ONNX models looking at the audio rather than
    // anything Whisper does -- see transcription/diarizer.hpp. Needs its models
    // downloaded once from Settings.
    bool diarize_far_end = false;
    std::string mic_source;            // empty = system default source
    std::string monitor_source;        // empty = "<default-sink>.monitor"
    std::string user_display_name = kDefaultUserDisplayName;
    std::string reminder_time = "09:00";
    bool reminder_enabled = true;
    bool echo_cancel = true;  // strip speaker bleed from the mic while recording

    // Mouse Mover: how often to nudge the pointer, and by how much. The nudge
    // is a small square that ends where it started, so the cursor never drifts.
    int mouse_mover_interval_seconds = 60;
    int mouse_mover_pixels = 20;
    bool mouse_mover_autostart = false;

    // Calendar import. Read-only and unauthenticated -- either a published
    // ICS feed URL ("url") or a hand-exported .ics on disk ("file"); "off"
    // hides the agenda. The URL is a bearer secret (anyone holding it can
    // read the calendar), which is why config.json is written owner-only.
    std::string calendar_source = "off";  // "off" | "url" | "file"
    std::string calendar_ics_url;
    std::string calendar_ics_path;
    int calendar_refresh_minutes = 30;

    // Editable prompt presets (see the k*Prompt defaults above).
    std::string summary_prompt = kDefaultSummaryPrompt;
    std::string email_prompt = kDefaultEmailPrompt;
    std::string tasks_prompt = kDefaultTasksPrompt;

    static Settings with_defaults();
};

Settings load_settings();
void save_settings(const Settings& settings);
void ensure_data_dirs();

}  // namespace callscribe
