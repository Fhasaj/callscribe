#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace callscribe {

namespace {
std::filesystem::path env_or(const char* var, const std::filesystem::path& fallback) {
    const char* v = std::getenv(var);
    return (v && *v) ? std::filesystem::path(v) : fallback;
}
std::filesystem::path home() {
    const char* h = std::getenv("HOME");
    return h ? std::filesystem::path(h) : std::filesystem::path("/tmp");
}
}  // namespace

std::filesystem::path config_dir() {
    return env_or("XDG_CONFIG_HOME", home() / ".config") / "callscribe-cpp";
}
std::filesystem::path config_file() { return config_dir() / "config.json"; }

std::filesystem::path data_dir() {
    return env_or("XDG_DATA_HOME", home() / ".local" / "share") / "callscribe-cpp";
}
std::filesystem::path recordings_dir() { return data_dir() / "recordings"; }
std::filesystem::path models_dir() { return data_dir() / "models"; }
std::filesystem::path default_db_path() { return data_dir() / "callscribe.db"; }

Settings Settings::with_defaults() {
    Settings s;
    s.db_path = default_db_path().string();
    return s;
}

Settings load_settings() {
    Settings defaults = Settings::with_defaults();
    std::ifstream in(config_file());
    if (!in) return defaults;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::exception&) {
        return defaults;
    }

    Settings s = defaults;
    if (j.contains("db_path")) s.db_path = j.at("db_path").get<std::string>();
    if (j.contains("ollama_host")) s.ollama_host = j.at("ollama_host").get<std::string>();
    if (j.contains("ollama_model")) s.ollama_model = j.at("ollama_model").get<std::string>();
    if (j.contains("whisper_model_size")) s.whisper_model_size = j.at("whisper_model_size").get<std::string>();
    if (j.contains("mic_source")) s.mic_source = j.at("mic_source").get<std::string>();
    if (j.contains("monitor_source")) s.monitor_source = j.at("monitor_source").get<std::string>();
    if (j.contains("user_display_name")) s.user_display_name = j.at("user_display_name").get<std::string>();
    if (j.contains("reminder_time")) s.reminder_time = j.at("reminder_time").get<std::string>();
    if (j.contains("reminder_enabled")) s.reminder_enabled = j.at("reminder_enabled").get<bool>();
    return s;
}

void save_settings(const Settings& s) {
    std::filesystem::create_directories(config_dir());
    nlohmann::json j;
    j["db_path"] = s.db_path;
    j["ollama_host"] = s.ollama_host;
    j["ollama_model"] = s.ollama_model;
    j["whisper_model_size"] = s.whisper_model_size;
    j["mic_source"] = s.mic_source;
    j["monitor_source"] = s.monitor_source;
    j["user_display_name"] = s.user_display_name;
    j["reminder_time"] = s.reminder_time;
    j["reminder_enabled"] = s.reminder_enabled;
    std::ofstream out(config_file());
    out << j.dump(2);
}

void ensure_data_dirs() {
    std::filesystem::create_directories(data_dir());
    std::filesystem::create_directories(recordings_dir());
    std::filesystem::create_directories(models_dir());
}

}  // namespace callscribe
