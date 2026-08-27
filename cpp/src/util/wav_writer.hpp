#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace callscribe::util {

// Writes a 16-bit PCM mono WAV file. Throws std::runtime_error on failure.
// Takes the samples as int16 because that is what the capture buffers already
// hold -- no float round trip, and no second full-length allocation for a
// recording that can run to hundreds of MB.
void write_wav_mono16(const std::string& path, const std::vector<int16_t>& samples, int sample_rate);

}  // namespace callscribe::util
