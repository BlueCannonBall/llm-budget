#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class Channel {
protected:
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<T> messages;

public:
    void send(T message) {
        std::unique_lock<std::mutex> lock(mutex);
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
};
