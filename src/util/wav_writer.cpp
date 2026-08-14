#include "util/wav_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace callscribe::util {

namespace {
void write_u32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void write_u16(std::ofstream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
}  // namespace

void write_wav_mono16(const std::string& path, const std::vector<float>& samples, int sample_rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to open " + path + " for writing");

    uint32_t data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint32_t byte_rate = static_cast<uint32_t>(sample_rate) * 2;

    out.write("RIFF", 4);
    write_u32(out, 36 + data_bytes);
    out.write("WAVE", 4);

    out.write("fmt ", 4);
    write_u32(out, 16);  // PCM header size
    write_u16(out, 1);   // PCM format
    write_u16(out, 1);   // mono
    write_u32(out, static_cast<uint32_t>(sample_rate));
    write_u32(out, byte_rate);
    write_u16(out, 2);   // block align
    write_u16(out, 16);  // bits per sample

    out.write("data", 4);
    write_u32(out, data_bytes);

    for (float sample : samples) {
        float clamped = std::max(-1.0f, std::min(1.0f, sample));
        int16_t pcm = static_cast<int16_t>(clamped * 32767.0f);
        out.write(reinterpret_cast<const char*>(&pcm), 2);
    }
}

}  // namespace callscribe::util
