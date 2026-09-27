#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class Channel {
protected:
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::queue<T> messages;
    size_t max_queue_size;

public:
    Channel(size_t max_queue_size = 0):
        max_queue_size(max_queue_size) {}

    void send(T message) {
        std::unique_lock<std::mutex> lock(mutex);
        if (max_queue_size) {
            cv.wait(lock, [this]() {
                return messages.size() < max_queue_size;
            });
        }
        messages.push(std::move(message));
        lock.unlock();
        cv.notify_one();
    }

    T recv() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() {
            return !messages.empty();
        });
        T message = std::move(messages.front());
        messages.pop();
        return message;
    }

    size_t get_max_queue_size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return max_queue_size;
    }

    void set_max_queue_size(size_t value) {
        std::lock_guard<std::mutex> lock(mutex);
        max_queue_size = value;
    }
};
