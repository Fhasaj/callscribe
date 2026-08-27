#include "audio/capture.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace callscribe::audio {

namespace {
struct Spawned {
    pid_t pid;
    int read_fd;
};

Spawned spawn_parec(const std::string& source) {
    int fds[2];
    if (pipe(fds) != 0) {
        throw std::runtime_error("pipe() failed while starting audio capture");
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        throw std::runtime_error("fork() failed while starting audio capture");
    }
    if (pid == 0) {
        // child: stdout -> pipe, stderr -> /dev/null, exec parec
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        std::vector<std::string> args = {"parec", "--format=s16le", "--rate=16000", "--channels=1", "--raw"};
        if (!source.empty()) args.push_back("--device=" + source);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp("parec", argv.data());
        _exit(127);  // exec failed
    }
    close(fds[1]);
    return {pid, fds[0]};
}
}  // namespace

StreamCapture::StreamCapture(std::string source) : source_(std::move(source)) {
    chunk_queue_.set_capacity(kMaxQueuedChunks);
}

StreamCapture::~StreamCapture() {
    if (pid_ > 0) stop();
}

void StreamCapture::start() {
    Spawned spawned = spawn_parec(source_);
    pid_ = spawned.pid;
    read_fd_ = spawned.read_fd;
    reader_thread_ = std::thread(&StreamCapture::reader_loop, this);
}

void StreamCapture::reader_loop() {
    // 100ms read granularity -- small enough that current_level() feels
    // instant in the UI, but still cheap at 16kHz mono. Whisper chunking
    // (kChunkSeconds) is unaffected: samples still accumulate into
    // pending_/full_audio_ regardless of how finely they arrive.
    constexpr size_t block_samples = static_cast<size_t>(kSampleRate) / 10;
    constexpr size_t block_bytes = block_samples * sizeof(int16_t);
    const size_t chunk_target_samples = static_cast<size_t>(kSampleRate * kChunkSeconds);

    std::vector<char> buf(block_bytes);
    while (true) {
        size_t total_read = 0;
        while (total_read < block_bytes) {
            ssize_t n = ::read(read_fd_, buf.data() + total_read, block_bytes - total_read);
            if (n <= 0) break;
            total_read += static_cast<size_t>(n);
        }
        if (total_read == 0) break;

        // Paused: the block was still read (parec's pipe must keep draining or
        // it blocks and resuming would replay stale audio), but it is dropped.
        // The pending chunk is flushed on the way in so the utterance that was
        // in progress still gets transcribed rather than being stitched onto
        // whatever is said after the resume.
        if (paused_.load()) {
            current_level_.store(0.0f);
            flush_pending();
            if (total_read < block_bytes) break;
            continue;
        }

        size_t n_samples = total_read / sizeof(int16_t);
        const int16_t* pcm = reinterpret_cast<const int16_t*>(buf.data());

        // The archive copy keeps the int16 parec gave us; only the Whisper
        // chunk needs floats, and it is appended in the same pass.
        full_audio_.insert(full_audio_.end(), pcm, pcm + n_samples);
        size_t pending_before = pending_.size();
        pending_.resize(pending_before + n_samples);
        double sum_sq = 0.0;
        for (size_t i = 0; i < n_samples; ++i) {
            float sample = pcm[i] / 32768.0f;
            pending_[pending_before + i] = sample;
            sum_sq += static_cast<double>(sample) * sample;
        }
        current_level_.store(
            n_samples ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n_samples))) : 0.0f);

        if (pending_.size() >= chunk_target_samples) flush_pending();

        if (total_read < block_bytes) break;  // short read means EOF followed
    }
    flush_pending();
    chunk_queue_.close();
}

void StreamCapture::flush_pending() {
    if (pending_.empty()) return;
    AudioChunk chunk;
    // pending_ always holds exactly the trailing samples of full_audio_ -- both
    // are appended together from the same block -- so the chunk's offset is
    // where those samples begin in the recording.
    chunk.start_sample = static_cast<int64_t>(full_audio_.size() - pending_.size());
    chunk.samples = std::move(pending_);
    pending_.clear();
    chunk_queue_.push(std::move(chunk));
}

std::vector<int16_t> StreamCapture::stop() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    if (reader_thread_.joinable()) reader_thread_.join();
    if (read_fd_ >= 0) {
        close(read_fd_);
        read_fd_ = -1;
    }
    return std::move(full_audio_);
}

}  // namespace callscribe::audio
