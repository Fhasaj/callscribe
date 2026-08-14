#include "config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
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

std::vector<std::string> default_report_kpis() {
    return {
        "Calls completed",
        "Total time spent on calls",
        "Customer follow-ups sent",
        "Action items completed",
        "Accounts engaged",
    };
}

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
    // An explicitly empty KPI list is a valid choice (the report then just
    // covers the activity as a whole), so only a missing/malformed key falls
    // back to the defaults.
    if (j.contains("report_kpis") && j.at("report_kpis").is_array()) {
        std::vector<std::string> kpis;
        for (const auto& k : j.at("report_kpis")) {
            if (!k.is_string()) continue;
            std::string v = k.get<std::string>();
            if (!v.empty()) kpis.push_back(std::move(v));
        }
        s.report_kpis = std::move(kpis);
    }
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
    j["echo_cancel"] = s.echo_cancel;
    j["mouse_mover_interval_seconds"] = s.mouse_mover_interval_seconds;
    j["mouse_mover_pixels"] = s.mouse_mover_pixels;
    j["mouse_mover_autostart"] = s.mouse_mover_autostart;
    j["calendar_source"] = s.calendar_source;
    j["calendar_ics_url"] = s.calendar_ics_url;
    j["calendar_ics_path"] = s.calendar_ics_path;
    j["calendar_refresh_minutes"] = s.calendar_refresh_minutes;
    j["summary_prompt"] = s.summary_prompt;
    j["email_prompt"] = s.email_prompt;
    j["tasks_prompt"] = s.tasks_prompt;
    j["report_kpis"] = s.report_kpis;
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
