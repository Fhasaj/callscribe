#pragma once
#include <string>

namespace callscribe::audio {

// Acoustic echo cancellation for the "my mic picks up the speakers" problem,
// using PipeWire's WebRTC echo-cancel module.
//
// The module creates a paired virtual sink and source. Audio played into the
// sink is both forwarded to the real speakers AND handed to the canceller as
// the reference signal, which it then subtracts from the mic. That reference is
// the whole point: for cancellation to happen the far end's audio has to travel
// through the virtual sink, so while a session is active the default sink is
// switched to it (and restored afterwards) -- otherwise the canceller has
// nothing to subtract and the mic is passed through untouched.
//
// A side benefit: the virtual sink's monitor carries exactly the far-end audio,
// which is a cleaner "them" stream than the system-wide output monitor.
//
// RAII -- the destructor restores the previous default sink and unloads the
// module. Construction never throws; check `active()` and `error()`.
class EchoCancelSession {
public:
    // `mic_source` is the real microphone to clean up ("" = system default).
    explicit EchoCancelSession(const std::string& mic_source);
    ~EchoCancelSession();
    EchoCancelSession(const EchoCancelSession&) = delete;
    EchoCancelSession& operator=(const EchoCancelSession&) = delete;

    bool active() const { return module_id_ >= 0; }
    const std::string& error() const { return error_; }

    // Sources to capture from. When inactive these are empty and callers should
    // fall back to their configured devices.
    const std::string& mic_source() const { return aec_source_; }
    const std::string& monitor_source() const { return aec_monitor_; }

private:
    long module_id_ = -1;
    std::string aec_source_;
    std::string aec_monitor_;
    std::string previous_default_sink_;
    std::string error_;
};

// True if the PipeWire echo-cancel module is installed and loadable.
bool echo_cancel_available();

}  // namespace callscribe::audio
