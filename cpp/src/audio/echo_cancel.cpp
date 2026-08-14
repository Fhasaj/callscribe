#include "audio/echo_cancel.hpp"

#include <filesystem>

#include "util/subprocess.hpp"

namespace callscribe::audio {

namespace {

constexpr const char* kSourceName = "callscribe_aec_source";
constexpr const char* kSinkName = "callscribe_aec_sink";

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Shell-quote so a device name with spaces or quotes can't break the command.
std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

}  // namespace

bool echo_cancel_available() {
    std::error_code ec;
    for (const char* dir : {"/usr/lib/pipewire-0.3", "/usr/lib64/pipewire-0.3", "/usr/local/lib/pipewire-0.3"}) {
        if (std::filesystem::exists(std::string(dir) + "/libpipewire-module-echo-cancel.so", ec)) return true;
    }
    // PulseAudio proper ships its own equivalent under a different path; if we
    // can't find either, loading will fail and we'll report that instead.
    return std::filesystem::exists("/usr/lib/pulse-17.0/modules/module-echo-cancel.so", ec);
}

EchoCancelSession::EchoCancelSession(const std::string& mic_source) {
    if (!echo_cancel_available()) {
        error_ = "PipeWire's echo-cancel module isn't installed -- install pipewire-audio (or pulseaudio-echo-cancel).";
        return;
    }

    std::string args = std::string("source_name=") + kSourceName + " sink_name=" + kSinkName + " aec_method=webrtc";
    if (!mic_source.empty()) args += " source_master=" + quote(mic_source);

    std::string out = trim(util::run_capture("pactl load-module module-echo-cancel " + args + " 2>&1"));
    if (!is_number(out)) {
        error_ = out.empty() ? "Could not load the echo-cancel module." : ("Echo cancellation failed: " + out);
        return;
    }
    module_id_ = std::stol(out);
    aec_source_ = kSourceName;
    aec_monitor_ = std::string(kSinkName) + ".monitor";

    // Route system audio through the virtual sink so the canceller has a
    // reference to subtract; without this the mic passes through unchanged.
    previous_default_sink_ = trim(util::run_capture("pactl get-default-sink 2>/dev/null"));
    util::run_capture("pactl set-default-sink " + std::string(kSinkName) + " >/dev/null 2>&1");
}

EchoCancelSession::~EchoCancelSession() {
    if (module_id_ < 0) return;
    if (!previous_default_sink_.empty()) {
        util::run_capture("pactl set-default-sink " + quote(previous_default_sink_) + " >/dev/null 2>&1");
    }
    util::run_capture("pactl unload-module " + std::to_string(module_id_) + " >/dev/null 2>&1");
}

}  // namespace callscribe::audio
