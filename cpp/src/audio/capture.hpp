#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "util/blocking_queue.hpp"

namespace callscribe::audio {

inline constexpr int kSampleRate = 16000;
inline constexpr double kChunkSeconds = 12.0;  // audio handed to Whisper per chunk

// Cap on un-transcribed chunks held in memory. Whisper on a GPU runs faster
// than realtime and never gets near this; on the CPU fallback it doesn't, and
// the backlog would otherwise grow for the whole call (~770 KB per chunk).
// 150 chunks is half an hour of slack, ~115 MB, past which the oldest are
// dropped from the live transcript. The full recording is unaffected -- it is
// accumulated separately and written to the WAV in one piece.
inline constexpr size_t kMaxQueuedChunks = 150;

// One ~kChunkSeconds slice of audio handed to Whisper, tagged with where it
// sits in the stream.
//
// The offset matters because speaker diarization runs on the whole recording
// afterwards and reports speaker turns as time ranges: without knowing when
// each transcribed chunk started, there is no way to line its text up against
// those ranges. It counts only samples that were actually kept, so a paused
// stretch advances neither the recording nor this clock and the two stay in
// step.
struct AudioChunk {
    std::vector<float> samples;
    int64_t start_sample = 0;
};

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
    //
    // int16, not float: this buffer grows for the whole call and is the
    // largest thing the app holds -- ~115 MB per recorded hour per stream,
    // where float32 was ~230 MB. It is only ever written to a 16-bit WAV, so
    // keeping it as the samples parec already hands us is lossless as well as
    // half the size. (The chunks fed to Whisper are still float; those are
    // ~12 seconds each and get released as soon as they are transcribed.)
    std::vector<int16_t> stop();

    // While paused, parec keeps running and its output keeps being drained
    // (so the pipe never backs up and resuming is instant), but the samples
    // are dropped: paused audio reaches neither the transcript nor the WAV.
    void set_paused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }

    util::BlockingQueue<AudioChunk>& chunks() { return chunk_queue_; }

    // Chunks discarded because transcription couldn't keep up (see
    // kMaxQueuedChunks). Non-zero means the live transcript has gaps.
    size_t dropped_chunks() const { return chunk_queue_.dropped(); }

    // Live input level (RMS of the most recent ~100ms block, roughly 0..1),
    // updated ~10x/second -- for driving a near-instant level gauge in the UI.
    float current_level() const { return current_level_.load(); }

private:
    std::string source_;
    pid_t pid_ = -1;
    int read_fd_ = -1;
    std::thread reader_thread_;

    std::vector<int16_t> full_audio_;
    std::vector<float> pending_;
    mutable util::BlockingQueue<AudioChunk> chunk_queue_;

    std::atomic<float> current_level_{0.0f};
    std::atomic<bool> paused_{false};

    void reader_loop();
    // Pushes pending_ as a chunk tagged with its start offset, and clears it.
    void flush_pending();
};

}  // namespace callscribe::audio
