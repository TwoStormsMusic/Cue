#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>

namespace cue {

// Bounded lock-free queue with one consumer (the audio thread). Pushes from
// several non-real-time threads are serialised by a mutex on the producer
// side only, so the consumer never waits.
template <typename T, size_t Capacity>
class CommandQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push(const T& value) {
        std::lock_guard<std::mutex> lock(producerMutex_);
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity)
            return false;
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Lock-free and wait-free.
    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail == head)
            return std::nullopt;
        T value = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::mutex producerMutex_;
};

// Bounded lock-free queue with one producer and one consumer, neither of
// which ever waits: for the audio thread to tell the main thread something.
template <typename T, size_t Capacity>
class RingQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Producer only. False when full: the value is dropped.
    bool push(const T& value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= Capacity)
            return false;
        slots_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;
        T value = slots_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }

private:
    std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace cue
