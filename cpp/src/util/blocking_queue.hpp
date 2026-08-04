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
template <typename T>
class BlockingQueue {
public:
    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(value));
        }
        cv_.notify_one();
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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

}  // namespace callscribe::util
