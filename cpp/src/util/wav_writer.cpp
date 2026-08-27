#include "util/wav_writer.hpp"

#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace callscribe::util {

namespace {
void write_u32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }
void write_u16(std::ofstream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }

// Opens the file and lays down the 44-byte canonical WAV header for
// `sample_count` mono 16-bit samples, leaving the stream positioned at the
// start of the data chunk.
std::ofstream open_with_header(const std::string& path, size_t sample_count, int sample_rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Failed to open " + path + " for writing");

    uint32_t data_bytes = static_cast<uint32_t>(sample_count * sizeof(int16_t));
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
    return out;
}
}  // namespace

void write_wav_mono16(const std::string& path, const std::vector<int16_t>& samples, int sample_rate) {
    std::ofstream out = open_with_header(path, samples.size(), sample_rate);
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
    if (!out) throw std::runtime_error("Failed to write " + path);
}

}  // namespace callscribe::util
