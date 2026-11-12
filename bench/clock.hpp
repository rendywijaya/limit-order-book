// clock.hpp — the cheapest trustworthy timestamp on this CPU.
//
//   x86-64 : RDTSC, the invariant time-stamp counter (~0.3 ns per tick at 3 GHz).
//            LFENCE before it stops the read from executing ahead of the work.
//   AArch64: CNTVCT_EL0, the generic timer's virtual count. On Apple M-series
//            it runs at 24 MHz, so ONE TICK IS 41.67 ns. Anything faster than
//            that measures as 0 or 1 tick; read the distributions accordingly.
//            ISB keeps the read from being reordered with the work.
//   other  : std::chrono::steady_clock.
//
// Ticks are converted to nanoseconds with a frequency calibrated against
// steady_clock at start-up, and the resolution is printed with every result.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace lob::bench {

inline std::uint64_t ticks() noexcept {
#if defined(__x86_64__)
    std::uint32_t lo, hi;
    asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return (std::uint64_t(hi) << 32) | lo;
#elif defined(__aarch64__)
    std::uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#else
    return std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

inline const char* clock_name() noexcept {
#if defined(__x86_64__)
    return "rdtsc";
#elif defined(__aarch64__)
    return "cntvct_el0";
#else
    return "steady_clock";
#endif
}

// Nanoseconds per tick, measured over ~200 ms against steady_clock.
inline double calibrate_ns_per_tick() {
    using namespace std::chrono;
    auto t0 = steady_clock::now();
    std::uint64_t c0 = ticks();
    std::this_thread::sleep_for(milliseconds(200));
    auto t1 = steady_clock::now();
    std::uint64_t c1 = ticks();
    return double(duration_cast<nanoseconds>(t1 - t0).count()) / double(c1 - c0);
}

}  // namespace lob::bench
