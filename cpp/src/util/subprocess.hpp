#pragma once
#include <string>

namespace callscribe::util {

// Runs `cmd` via the shell and captures stdout. Returns "" on failure.
// Only for short-lived, one-shot commands (e.g. `pactl ... list sources`)
// -- long-running processes (parec) are spawned directly via fork/exec,
// see audio/capture.cpp, since we need the child pid to terminate it.
std::string run_capture(const std::string& cmd);

}  // namespace callscribe::util
