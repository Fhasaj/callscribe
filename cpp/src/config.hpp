#pragma once
#include <filesystem>
#include <string>

namespace callscribe {

inline const char* kOllamaHostDefault = "http://localhost:11434";
inline const char* kRecommendedOllamaModel = "qwen2.5:7b-instruct";
inline const char* kDefaultUserDisplayName = "Fatlum Hasaj";
inline const char* kDefaultOtherSpeakerLabel = "Other";

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
    std::string whisper_model_size;    // empty = auto-suggest ("small" CPU / "medium" CUDA)
    std::string mic_source;            // empty = system default source
    std::string monitor_source;        // empty = "<default-sink>.monitor"
    std::string user_display_name = kDefaultUserDisplayName;
    std::string reminder_time = "09:00";
    bool reminder_enabled = true;

    static Settings with_defaults();
};

Settings load_settings();
void save_settings(const Settings& settings);
void ensure_data_dirs();

}  // namespace callscribe
