#pragma once
#include <algorithm>

namespace callscribe::ui {

// Converts a StreamCapture::current_level() RMS reading (on a normalized
// [-1,1] signal, so normal speech sits quiet at ~0.02-0.1) into a 0..1
// ftxui::gauge() progress value. Scaled up so a gauge isn't visually silent
// during normal speech.
inline float level_to_progress(float rms) { return std::clamp(rms * 6.0f, 0.0f, 1.0f); }

}  // namespace callscribe::ui
