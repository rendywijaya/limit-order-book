// spsc.hpp — a single-producer single-consumer ring, for the feed-to-strategy hop.
//
// One thread pushes, one thread pops; nothing else is allowed, and that is
// what makes it cheap. The two indices live on separate cache lines so the
// producer's writes to `tail` do not invalidate the consumer's line holding
// `head` (false sharing). Each side keeps a cached copy of the other's index
// and re-reads the shared one only when the cache says full or empty, so in
// steady state a push or a pop touches one shared line, not two.
//
//   push: store the item, then release-store tail   (the item is visible before the index)
//   pop : acquire-load tail, read the item, release-store head
//
// Capacity is a power of two; one slot is left empty to tell full from empty.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace lob {

template <class T, std::size_t CapacityPow2>
class SpscRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "items are copied as bytes");

public:
    SpscRing() : buf_(CapacityPow2) {}

    // Returns false if full (the item is not stored).
    bool push(const T& item) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (t + 1) & kMask;
        if (next == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (next == head_cache_) return false;
        }
        buf_[t] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Returns false if empty.
    bool pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = buf_[h];
        head_.store((h + 1) & kMask, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return CapacityPow2 - 1; }

private:
    static constexpr std::size_t kMask = CapacityPow2 - 1;
    static constexpr std::size_t kLine = 64;

    std::vector<T> buf_;
    alignas(kLine) std::atomic<std::size_t> head_{0};   // consumer writes
    alignas(kLine) std::size_t tail_cache_ = 0;         // consumer's copy of tail
    alignas(kLine) std::atomic<std::size_t> tail_{0};   // producer writes
    alignas(kLine) std::size_t head_cache_ = 0;         // producer's copy of head
};

}  // namespace lob
