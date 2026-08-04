#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "util/blocking_queue.hpp"

namespace callscribe::audio {

inline constexpr int kSampleRate = 16000;
inline constexpr double kChunkSeconds = 12.0;  // audio handed to Whisper per chunk

// Captures one named PipeWire/PulseAudio source via `parec` (spawned
// directly with fork/exec so we retain the child pid to terminate it),
// converts to mono float32 @ 16kHz, and exposes ~kChunkSeconds chunks via
// a blocking queue. Deliberately NOT mixed with any other stream -- the
// app runs one StreamCapture for the mic and one for the loopback/monitor
// source so each can be transcribed and speaker-labeled independently.
class StreamCapture {
public:
    explicit StreamCapture(std::string source);
    ~StreamCapture();
    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    void start();  // throws std::runtime_error if parec can't be spawned
    // Terminates parec, joins the reader thread, and returns every sample
    // captured (for archival WAV writing). Closes the chunk queue.
    std::vector<float> stop();

    util::BlockingQueue<std::vector<float>>& chunks() { return chunk_queue_; }

    // Live input level (RMS of the most recent ~100ms block, roughly 0..1),
    // updated ~10x/second -- for driving a near-instant level gauge in the UI.
    float current_level() const { return current_level_.load(); }

private:
    std::string source_;
    pid_t pid_ = -1;
    int read_fd_ = -1;
    std::thread reader_thread_;

    std::vector<float> full_audio_;
    std::vector<float> pending_;
    util::BlockingQueue<std::vector<float>> chunk_queue_;

    std::atomic<float> current_level_{0.0f};

    void reader_loop();
};

}  // namespace callscribe::audio
