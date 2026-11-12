// flat_hash_map.hpp — OrderId -> uint32 index, open addressing, linear probing.
//
// Why not std::unordered_map? It is a vector of buckets, each a linked list of
// separately allocated nodes. A lookup is a hash, a bucket load, then a pointer
// chase to a node somewhere else on the heap — often two cache misses. Here the
// keys and values sit side by side in one array: a lookup is a hash and (almost
// always) one cache line.
//
//  * Capacity is a power of two, so `hash & mask` replaces a division.
//  * Fibonacci hashing (multiply by 2^64/phi, take the top bits) spreads the
//    sequential ids that exchanges hand out across the whole table.
//  * Load factor <= 1/2 keeps probe sequences short.
//  * Erase uses backward-shift deletion instead of tombstones, so the table
//    never fills with dead slots and probes never get longer over a day.
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace lob {

class FlatHashMap {
public:
    static constexpr std::uint32_t kEmpty = UINT32_MAX;  // marks an empty slot (by value)

    explicit FlatHashMap(std::size_t expected = 1024) { rehash(std::bit_ceil(expected * 2 < 16 ? 16 : expected * 2)); }

    // Returns the value, or kEmpty if absent.
    std::uint32_t find(std::uint64_t key) const noexcept {
        for (std::size_t i = slot_of(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kEmpty) return kEmpty;
            if (s.key == key) return s.value;
        }
    }

    bool contains(std::uint64_t key) const noexcept { return find(key) != kEmpty; }

    // Inserts key -> value. The caller guarantees the key is absent.
    void insert(std::uint64_t key, std::uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);  // rare: only past `expected`
        place(key, value);
        ++size_;
    }

    // Removes key if present. Backward-shift deletion: after emptying slot i,
    // walk forward and pull back any entry that is displaced past the hole, so
    // every remaining key is still reachable from its home slot without gaps.
    void erase(std::uint64_t key) noexcept {
        std::size_t i = slot_of(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].value == kEmpty) return;
            if (slots_[i].key == key) break;
        }
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (slots_[j].value == kEmpty) break;
            std::size_t home = slot_of(slots_[j].key);
            // Move j into the hole if its home is not in the cyclic range (hole, j].
            bool in_range = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
            if (!in_range) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kEmpty;
        --size_;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        std::uint64_t key;
        std::uint32_t value;
    };

    std::size_t slot_of(std::uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    void place(std::uint64_t key, std::uint32_t value) noexcept {
        std::size_t i = slot_of(key);
        while (slots_[i].value != kEmpty) i = (i + 1) & mask_;
        slots_[i] = {key, value};
    }

    void rehash(std::size_t cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(cap, Slot{0, kEmpty});
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
        for (const Slot& s : old)
            if (s.value != kEmpty) place(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t       mask_ = 0;
    int               shift_ = 64;
    std::size_t       size_ = 0;
};

}  // namespace lob
