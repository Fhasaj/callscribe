#include "audio/devices.hpp"

#include <nlohmann/json.hpp>

#include "util/subprocess.hpp"

namespace callscribe::audio {

namespace {
std::string trim(std::string s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}
}  // namespace

std::vector<AudioSource> list_sources() {
    std::string out = util::run_capture("pactl -f json list sources 2>/dev/null");
    std::vector<AudioSource> sources;
    if (out.empty()) return sources;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(out);
    } catch (const nlohmann::json::exception&) {
        return sources;
    }
    if (!j.is_array()) return sources;
    for (const auto& entry : j) {
        std::string name = entry.value("name", "");
        std::string description = entry.value("description", name);
        bool is_monitor = name.size() >= 8 && name.substr(name.size() - 8) == ".monitor";
        sources.push_back(AudioSource{name, description, is_monitor});
    }
    return sources;
}

std::vector<AudioSource> list_mic_sources() {
    std::vector<AudioSource> out;
    for (auto& s : list_sources())
        if (!s.is_monitor) out.push_back(s);
    return out;
}

std::vector<AudioSource> list_monitor_sources() {
    std::vector<AudioSource> out;
    for (auto& s : list_sources())
        if (s.is_monitor) out.push_back(s);
    return out;
}

std::string default_mic_source() { return trim(util::run_capture("pactl get-default-source 2>/dev/null")); }

std::string default_monitor_source() {
    std::string sink = trim(util::run_capture("pactl get-default-sink 2>/dev/null"));
    return sink.empty() ? "" : sink + ".monitor";
}

bool audio_backend_available() { return !trim(util::run_capture("pactl info 2>/dev/null")).empty(); }

}  // namespace callscribe::audio
