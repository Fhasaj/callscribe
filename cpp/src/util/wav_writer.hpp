#pragma once
#include <string>
#include <vector>

namespace callscribe::util {

// Writes a 16-bit PCM mono WAV file. Throws std::runtime_error on failure.
void write_wav_mono16(const std::string& path, const std::vector<float>& samples, int sample_rate);

}  // namespace callscribe::util
