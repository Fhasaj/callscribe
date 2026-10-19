#pragma once
#include <filesystem>
#include <string>

namespace callscribe {

inline const char* kOllamaHostDefault = "http://localhost:11434";
// llama-server's own default.
inline const char* kLlamaCppHostDefault = "http://127.0.0.1:8080";
// Keyring entry (util/keyring.hpp) holding llama-server's --api-key.
inline const char* kLlamaCppApiKeySecret = "llamacpp-api-key";
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
    "List the concrete action items from the call, one entry per action.";

std::filesystem::path config_dir();
std::filesystem::path config_file();
std::filesystem::path data_dir();
std::filesystem::path recordings_dir();
std::filesystem::path models_dir();
std::filesystem::path default_db_path();

struct Settings {
    std::string db_path;
    // "ollama" or "llamacpp" -- which server every AI feature talks to.
    std::string llm_backend = "ollama";
    std::string llamacpp_host = kLlamaCppHostDefault;
    // What llama-server reports it has loaded; informational, since a
    // single-model server answers whatever name it is sent.
    std::string llamacpp_model;
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
    // Desktop notification this many minutes before each calendar meeting;
    // 0 turns it off.
    int meeting_reminder_minutes = 15;
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

    // Focus (Pomodoro) timer, in minutes. The long break comes after every
    // fourth focus round.
    int focus_minutes = 25;
    int short_break_minutes = 5;
    int long_break_minutes = 15;

    // Hyperfocus check-in: a nudge after this many minutes of focus rounds or
    // call recording with no 5-minute gap. 0 turns it off.
    int hyperfocus_nudge_minutes = 90;
    // End-of-day wrap-up reminder (HH:MM), and whether to send it.
    std::string end_of_day_time = "17:30";
    bool end_of_day_enabled = true;

    // Spotify, optional. The client ID is from your own app at
    // developer.spotify.com and is not a secret -- the login uses PKCE, so
    // there is no client secret at all. The refresh token it produces IS a
    // secret and never lands in this file: it lives in the desktop keyring
    // (see spotify/keyring.hpp). Empty client ID = Spotify off.
    std::string spotify_client_id;
    // The redirect URI registered on that app must be exactly
    // http://127.0.0.1:<port>/callback.
    int spotify_redirect_port = 8888;
    // Started when a focus round starts. A spotify:playlist:... URI or an
    // open.spotify.com/playlist/... link.
    std::string spotify_playlist_uri;

    // Editable prompt presets (see the k*Prompt defaults above).
    std::string summary_prompt = kDefaultSummaryPrompt;
    std::string email_prompt = kDefaultEmailPrompt;
    std::string tasks_prompt = kDefaultTasksPrompt;

    // Host and model for the backend in use. Every LLM call goes through these
    // rather than ollama_host/ollama_model directly.
    bool uses_llamacpp() const { return llm_backend == "llamacpp"; }
    std::string llm_host() const { return uses_llamacpp() ? llamacpp_host : ollama_host; }
    std::string llm_model() const {
        if (!uses_llamacpp()) return ollama_model;
        return llamacpp_model.empty() ? std::string("llama.cpp") : llamacpp_model;
    }

    static Settings with_defaults();
};

Settings load_settings();
void save_settings(const Settings& settings);
void ensure_data_dirs();

}  // namespace callscribe
