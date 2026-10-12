// ============================================================================
// SpscRing — lock-free single-producer/single-consumer ring.
// Producer (capture thread) -> consumer (engine IPC thread) chunk hand-off.
// When full it overwrites the oldest entry (drop-oldest): for a streaming
// ASR input, losing frames beats accumulating latency (same philosophy as
// the XRUN policy in the survey).
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace astk {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity_hint) {
        cap_ = 1;
        while (cap_ < capacity_hint) cap_ <<= 1;
        mask_ = cap_ - 1;
        slots_.resize(cap_);
    }

    // true: normal, false: overwrote an unread slot (a drop happened)
    bool push(T v) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_relaxed);
        bool dropped = (tail - head) >= cap_;
        slots_[tail & mask_] = std::move(v);
        tail_.store(tail + 1, std::memory_order_release);
        return !dropped;
    }

    bool pop(T& out) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (tail == head) return false;
        if (tail - head > cap_) head = tail - cap_; // resync after overwrite
        out = std::move(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t head = head_.load(std::memory_order_acquire);
        size_t n = tail > head ? tail - head : 0;
        return n > cap_ ? cap_ : n;
    }

    size_t capacity() const { return cap_; }

private:
    size_t cap_ = 0, mask_ = 0;
    std::vector<T> slots_;
    alignas(64) std::atomic<size_t> head_ {0};
    alignas(64) std::atomic<size_t> tail_ {0};
    static_assert(std::atomic<size_t>::is_always_lock_free);
};

} // namespace astk
