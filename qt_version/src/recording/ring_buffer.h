#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <new>

// Lock-free Single-Producer Single-Consumer ring buffer.
// Capacity must be a power of 2.
template<typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    struct alignas(64) AlignedAtomic {
        std::atomic<size_t> value{0};
    };

    std::array<T, Capacity> buffer_;
    AlignedAtomic head_; // written by producer
    AlignedAtomic tail_; // written by consumer

public:
    bool tryPush(const T& item) {
        size_t head = head_.value.load(std::memory_order_relaxed);
        size_t next = (head + 1) & MASK;
        if (next == tail_.value.load(std::memory_order_acquire))
            return false; // full
        buffer_[head] = item;
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    bool tryPush(T&& item) {
        size_t head = head_.value.load(std::memory_order_relaxed);
        size_t next = (head + 1) & MASK;
        if (next == tail_.value.load(std::memory_order_acquire))
            return false;
        buffer_[head] = std::move(item);
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& item) {
        size_t tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == head_.value.load(std::memory_order_acquire))
            return false; // empty
        item = std::move(buffer_[tail]);
        tail_.value.store((tail + 1) & MASK, std::memory_order_release);
        return true;
    }
};
