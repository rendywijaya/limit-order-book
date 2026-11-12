// wire.hpp — reading and writing the bytes exchanges actually send.
//
// Every Nasdaq protocol in this repository (ITCH, MoldUDP64, OUCH,
// SoupBinTCP) is big-endian with fixed-width fields: integers of 2, 4, 6 or
// 8 bytes, and left-justified, space-padded alphanumeric fields. These helpers
// are the only place byte order is handled. They take and return plain
// integers, never pointers into structs, because the wire formats are not
// naturally aligned (a 6-byte timestamp at offset 5, for instance) and a
// struct overlay would be undefined behaviour on top of being wrong.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace lob::wire {

using Bytes = std::span<const std::uint8_t>;
using MutBytes = std::span<std::uint8_t>;

inline std::uint16_t be16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t(be16(p)) << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

inline void put16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v);
}
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16); p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v);
}
inline void put48(std::uint8_t* p, std::uint64_t v) noexcept {
    put16(p, std::uint16_t(v >> 32)); put32(p + 2, std::uint32_t(v));
}
inline void put64(std::uint8_t* p, std::uint64_t v) noexcept {
    put32(p, std::uint32_t(v >> 32)); put32(p + 4, std::uint32_t(v));
}

// Alphanumeric field: left-justified, padded with spaces to n bytes.
inline void put_alpha(std::uint8_t* p, std::size_t n, std::string_view s) noexcept {
    std::size_t k = s.size() < n ? s.size() : n;
    std::memcpy(p, s.data(), k);
    std::memset(p + k, ' ', n - k);
}
// The same field read back, without the padding.
inline std::string_view alpha(const std::uint8_t* p, std::size_t n) noexcept {
    while (n > 0 && p[n - 1] == ' ') --n;
    return {reinterpret_cast<const char*>(p), n};
}
// Numeric field written as ASCII digits, right-justified, space-padded (SoupBinTCP's sequence number).
inline void put_ascii_num(std::uint8_t* p, std::size_t n, std::uint64_t v) noexcept {
    std::memset(p, ' ', n);
    std::size_t i = n;
    do { p[--i] = std::uint8_t('0' + v % 10); v /= 10; } while (v && i);
}
inline std::uint64_t ascii_num(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] >= '0' && p[i] <= '9') v = v * 10 + (p[i] - '0');
    return v;
}

// An 8-byte stock symbol as a 64-bit key, for hashing and comparison.
inline std::uint64_t symbol_key(const std::uint8_t* p) noexcept { return be64(p); }
inline std::uint64_t symbol_key(std::string_view s) noexcept {
    std::uint8_t b[8];
    put_alpha(b, 8, s);
    return be64(b);
}

}  // namespace lob::wire
