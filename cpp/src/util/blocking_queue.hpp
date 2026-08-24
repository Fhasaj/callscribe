#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

namespace callscribe::util {

// Simple thread-safe queue. pop() blocks until an item is available or the
// queue is closed and drained, at which point it returns nullopt -- the
// producer-closes/consumer-drains pattern used by the audio and
// transcription worker threads.
//
// Optionally bounded. Blocking the producer is not an option here: it is the
// thread draining parec's pipe, so stalling it would make the kernel buffer
// fill and real audio be lost. Instead, past `capacity` the OLDEST queued item
// is discarded to make room -- the live transcript loses a stretch rather than
// the process growing without limit when the consumer can't keep up. `dropped`
// counts those, so the UI can say so.
template <typename T>
class BlockingQueue {
public:
    // 0 (the default) means unbounded.
    void set_capacity(size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = capacity;
    }

    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (capacity_ > 0 && queue_.size() >= capacity_) {
                queue_.pop();
                ++dropped_;
            }
            queue_.push(std::move(value));
        }
        cv_.notify_one();
    }

    // How many items were discarded because the consumer fell behind.
    size_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return std::nullopt;
        T value = std::move(queue_.front());
        queue_.pop();
        return value;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    size_t capacity_ = 0;
    size_t dropped_ = 0;
    bool closed_ = false;
};

}  // namespace callscribe::util
