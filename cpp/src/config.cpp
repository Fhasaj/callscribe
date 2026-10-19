#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <nlohmann/json.hpp>

#include "focus/pomodoro.hpp"

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
    if (j.contains("llm_backend") && j.at("llm_backend").is_string())
        s.llm_backend = j.at("llm_backend").get<std::string>() == "llamacpp" ? "llamacpp" : "ollama";
    if (j.contains("llamacpp_host") && j.at("llamacpp_host").is_string() &&
        !j.at("llamacpp_host").get<std::string>().empty())
        s.llamacpp_host = j.at("llamacpp_host").get<std::string>();
    if (j.contains("llamacpp_model") && j.at("llamacpp_model").is_string())
        s.llamacpp_model = j.at("llamacpp_model").get<std::string>();
    // An empty value would mean "send no keep_alive at all", handing control
    // back to the server's env var -- keep the default instead.
    if (j.contains("ollama_unload_after_use") && j.at("ollama_unload_after_use").is_boolean())
        s.ollama_unload_after_use = j.at("ollama_unload_after_use").get<bool>();
    if (j.contains("ollama_keep_alive") && j.at("ollama_keep_alive").is_string()) {
        std::string v = j.at("ollama_keep_alive").get<std::string>();
        if (!v.empty()) s.ollama_keep_alive = v;
    }
    if (j.contains("whisper_model_size")) s.whisper_model_size = j.at("whisper_model_size").get<std::string>();
    if (j.contains("detect_speaker_turns") && j.at("detect_speaker_turns").is_boolean())
        s.detect_speaker_turns = j.at("detect_speaker_turns").get<bool>();
    if (j.contains("diarize_far_end") && j.at("diarize_far_end").is_boolean())
        s.diarize_far_end = j.at("diarize_far_end").get<bool>();
    if (j.contains("mic_source")) s.mic_source = j.at("mic_source").get<std::string>();
    if (j.contains("monitor_source")) s.monitor_source = j.at("monitor_source").get<std::string>();
    if (j.contains("user_display_name")) s.user_display_name = j.at("user_display_name").get<std::string>();
    if (j.contains("reminder_time")) s.reminder_time = j.at("reminder_time").get<std::string>();
    if (j.contains("reminder_enabled")) s.reminder_enabled = j.at("reminder_enabled").get<bool>();
    if (j.contains("meeting_reminder_minutes") && j.at("meeting_reminder_minutes").is_number_integer())
        s.meeting_reminder_minutes = std::clamp(j.at("meeting_reminder_minutes").get<int>(), 0, 120);
    if (j.contains("echo_cancel")) s.echo_cancel = j.at("echo_cancel").get<bool>();
    if (j.contains("mouse_mover_interval_seconds"))
        s.mouse_mover_interval_seconds = j.at("mouse_mover_interval_seconds").get<int>();
    if (j.contains("mouse_mover_pixels")) s.mouse_mover_pixels = j.at("mouse_mover_pixels").get<int>();
    if (j.contains("mouse_mover_autostart")) s.mouse_mover_autostart = j.at("mouse_mover_autostart").get<bool>();
    if (j.contains("calendar_source")) s.calendar_source = j.at("calendar_source").get<std::string>();
    if (j.contains("calendar_ics_url")) s.calendar_ics_url = j.at("calendar_ics_url").get<std::string>();
    if (j.contains("calendar_ics_path")) s.calendar_ics_path = j.at("calendar_ics_path").get<std::string>();
    if (j.contains("calendar_refresh_minutes"))
        s.calendar_refresh_minutes = std::max(5, j.at("calendar_refresh_minutes").get<int>());
    auto load_minutes = [&j](const char* key, int& target) {
        if (j.contains(key) && j.at(key).is_number_integer())
            target = std::clamp(j.at(key).get<int>(), focus::kMinMinutes, focus::kMaxMinutes);
    };
    load_minutes("focus_minutes", s.focus_minutes);
    load_minutes("short_break_minutes", s.short_break_minutes);
    load_minutes("long_break_minutes", s.long_break_minutes);
    if (j.contains("hyperfocus_nudge_minutes") && j.at("hyperfocus_nudge_minutes").is_number_integer())
        s.hyperfocus_nudge_minutes = std::clamp(j.at("hyperfocus_nudge_minutes").get<int>(), 0, 600);
    if (j.contains("end_of_day_time") && j.at("end_of_day_time").is_string())
        s.end_of_day_time = j.at("end_of_day_time").get<std::string>();
    if (j.contains("end_of_day_enabled") && j.at("end_of_day_enabled").is_boolean())
        s.end_of_day_enabled = j.at("end_of_day_enabled").get<bool>();
    if (j.contains("spotify_client_id") && j.at("spotify_client_id").is_string())
        s.spotify_client_id = j.at("spotify_client_id").get<std::string>();
    if (j.contains("spotify_redirect_port") && j.at("spotify_redirect_port").is_number_integer())
        s.spotify_redirect_port = std::clamp(j.at("spotify_redirect_port").get<int>(), 1024, 65535);
    if (j.contains("spotify_playlist_uri") && j.at("spotify_playlist_uri").is_string())
        s.spotify_playlist_uri = j.at("spotify_playlist_uri").get<std::string>();
    // A blank preset would silently strip that section from the prompt, so fall
    // back to the default rather than trusting an empty string.
    auto load_prompt = [&j](const char* key, std::string& target) {
        if (!j.contains(key) || !j.at(key).is_string()) return;
        std::string v = j.at(key).get<std::string>();
        if (!v.empty()) target = v;
    };
    load_prompt("summary_prompt", s.summary_prompt);
    load_prompt("email_prompt", s.email_prompt);
    load_prompt("tasks_prompt", s.tasks_prompt);
    // The old default asked for tasks "mentioned or implied", which pulled in
    // other people's jobs and passing remarks. Saved configs carry it verbatim,
    // so move that exact text on; wording the user wrote themselves stays.
    if (s.tasks_prompt == "List the concrete action items mentioned or implied in the call, one entry per action.")
        s.tasks_prompt = kDefaultTasksPrompt;
    return s;
}

void save_settings(const Settings& s) {
    std::filesystem::create_directories(config_dir());
    nlohmann::json j;
    j["db_path"] = s.db_path;
    j["ollama_host"] = s.ollama_host;
    j["ollama_model"] = s.ollama_model;
    j["llm_backend"] = s.llm_backend;
    j["llamacpp_host"] = s.llamacpp_host;
    j["llamacpp_model"] = s.llamacpp_model;
    j["ollama_keep_alive"] = s.ollama_keep_alive;
    j["ollama_unload_after_use"] = s.ollama_unload_after_use;
    j["whisper_model_size"] = s.whisper_model_size;
    j["detect_speaker_turns"] = s.detect_speaker_turns;
    j["diarize_far_end"] = s.diarize_far_end;
    j["mic_source"] = s.mic_source;
    j["monitor_source"] = s.monitor_source;
    j["user_display_name"] = s.user_display_name;
    j["reminder_time"] = s.reminder_time;
    j["reminder_enabled"] = s.reminder_enabled;
    j["meeting_reminder_minutes"] = s.meeting_reminder_minutes;
    j["echo_cancel"] = s.echo_cancel;
    j["mouse_mover_interval_seconds"] = s.mouse_mover_interval_seconds;
    j["mouse_mover_pixels"] = s.mouse_mover_pixels;
    j["mouse_mover_autostart"] = s.mouse_mover_autostart;
    j["calendar_source"] = s.calendar_source;
    j["calendar_ics_url"] = s.calendar_ics_url;
    j["calendar_ics_path"] = s.calendar_ics_path;
    j["calendar_refresh_minutes"] = s.calendar_refresh_minutes;
    j["focus_minutes"] = s.focus_minutes;
    j["short_break_minutes"] = s.short_break_minutes;
    j["long_break_minutes"] = s.long_break_minutes;
    j["hyperfocus_nudge_minutes"] = s.hyperfocus_nudge_minutes;
    j["end_of_day_time"] = s.end_of_day_time;
    j["end_of_day_enabled"] = s.end_of_day_enabled;
    j["spotify_client_id"] = s.spotify_client_id;
    j["spotify_redirect_port"] = s.spotify_redirect_port;
    j["spotify_playlist_uri"] = s.spotify_playlist_uri;
    j["summary_prompt"] = s.summary_prompt;
    j["email_prompt"] = s.email_prompt;
    j["tasks_prompt"] = s.tasks_prompt;
    std::ofstream out(config_file());
    out << j.dump(2);
    out.close();
    // A published-calendar URL is a bearer token for the whole calendar, so
    // the file it lives in is kept owner-only rather than the 0644 an
    // ofstream would otherwise leave behind.
    std::error_code ec;
    std::filesystem::permissions(config_file(),
                                  std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                  std::filesystem::perm_options::replace, ec);
}

void ensure_data_dirs() {
    std::filesystem::create_directories(data_dir());
    std::filesystem::create_directories(recordings_dir());
    std::filesystem::create_directories(models_dir());
}

}  // namespace callscribe
