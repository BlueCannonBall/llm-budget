#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class Channel {
protected:
    mutable std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    std::queue<T> messages;
    size_t max_queue_size;

public:
    explicit Channel(size_t max_queue_size = 0):
        max_queue_size(max_queue_size) {}

    void send(T message) {
        std::unique_lock<std::mutex> lock(mutex);
        not_full.wait(lock, [this]() {
            return !max_queue_size || messages.size() < max_queue_size;
        });
        messages.push(std::move(message));
        lock.unlock();
        not_empty.notify_one();
    }

    T recv() {
        std::unique_lock<std::mutex> lock(mutex);
        not_empty.wait(lock, [this]() {
            return !messages.empty();
        });
        T message = std::move(messages.front());
        messages.pop();
        if (max_queue_size && messages.size() < max_queue_size) {
            lock.unlock();
            not_full.notify_one();
        }
        return message;
    }

    size_t get_max_queue_size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return max_queue_size;
    }

    void set_max_queue_size(size_t value) {
        std::unique_lock<std::mutex> lock(mutex);
        max_queue_size = value;
        lock.unlock();
        not_full.notify_all();
    }
};
