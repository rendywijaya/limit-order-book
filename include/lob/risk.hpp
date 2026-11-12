// risk.hpp — pre-trade risk: every order, every time, in a fixed order, in integers.
//
// The gate between the strategy and the exchange. A strategy bug, a bad
// feed, or a fat finger must not be able to send an order that could hurt
// the firm, and the check has to cost tens of nanoseconds because it runs on
// every message. So: no floating point, no allocation, no locks, a fixed
// sequence of integer comparisons against limits set before the day starts,
// and a kill switch in front of all of them.
//
// Two rules that are not obvious and that interviewers ask about:
//   * Cancels always pass. A cancel can only reduce exposure, and the moment
//     you most need one out is the moment something else is wrong.
//   * The position check is on the WORST CASE: current position plus every
//     open order on the same side, as if they all filled this instant.
#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

enum class RiskReason : std::uint8_t {
    Ok = 0,
    KillSwitch,    // everything but cancels is refused
    NoReference,   // no two-sided market to check the price against
    OrderSize,     // qty > max_order_qty
    PriceCollar,   // |price - mid| > collar
    Notional,      // price * qty > max_notional
    OpenOrders,    // too many live orders already
    Position,      // worst-case position would exceed the limit
    MessageRate,   // too many messages in the current second
};
constexpr const char* to_string(RiskReason r) noexcept {
    switch (r) {
        case RiskReason::Ok: return "ok";
        case RiskReason::KillSwitch: return "kill switch";
        case RiskReason::NoReference: return "no reference price";
        case RiskReason::OrderSize: return "order size";
        case RiskReason::PriceCollar: return "price collar";
        case RiskReason::Notional: return "notional";
        case RiskReason::OpenOrders: return "open orders";
        case RiskReason::Position: return "position";
        case RiskReason::MessageRate: return "message rate";
    }
    return "?";
}

struct RiskLimits {
    Qty           max_order_qty = 1000;
    Price         collar_ticks = 50;              // |price - mid| in ticks
    std::uint64_t max_notional = 100'000'000;     // price(ticks) * qty; 1e8 cents = $1,000,000
    std::uint32_t max_open_orders = 4;
    std::int64_t  max_position = 2000;            // |worst-case position|
    std::uint32_t max_msgs_per_sec = 500;
};

// What the firm currently has at risk in this symbol. Maintained by the order
// gateway from the exchange's answers; read by the gate.
struct Exposure {
    std::int64_t  position = 0;        // signed shares
    Qty           open_buy_qty = 0;    // resting buy quantity
    Qty           open_sell_qty = 0;   // resting sell quantity
    std::uint32_t open_orders = 0;
};

class RiskGate {
public:
    struct Stats {
        std::uint64_t checked = 0, passed = 0;
        std::uint64_t refused[9] = {};  // indexed by RiskReason
    };

    // The reference price the collar uses: the top of the market book.
    struct TopRef {
        bool  two_sided = false;
        Price mid2 = 0;  // bid + ask, in half-ticks, exact
    };

    explicit RiskGate(RiskLimits limits = {}) : lim_(limits) {}

    // A new order of `qty` at `price`, with the current mid in half-ticks
    // (bid + ask) for the collar. `now_ns` drives the message-rate window.
    RiskReason check_new(Side side, Price price, Qty qty, const TopRef& ref, const Exposure& x,
                         std::uint64_t now_ns) noexcept {
        return check(side, price, qty, /*releases=*/0, ref, x, now_ns);
    }
    // A replace: the order's open quantity is released and the new quantity
    // takes its place in the worst case.
    RiskReason check_replace(Side side, Price price, Qty new_qty, Qty open_qty, const TopRef& ref, const Exposure& x,
                             std::uint64_t now_ns) noexcept {
        return check(side, price, new_qty, open_qty, ref, x, now_ns);
    }
    // Cancels always pass. They are counted against the message rate (the
    // exchange counts them) but never refused for it.
    RiskReason check_cancel(std::uint64_t now_ns) noexcept {
        ++stats_.checked;
        ++stats_.passed;
        count_message(now_ns);
        return RiskReason::Ok;
    }

    void kill() noexcept { killed_ = true; }
    void arm() noexcept { killed_ = false; }
    bool killed() const noexcept { return killed_; }
    const Stats& stats() const noexcept { return stats_; }
    const RiskLimits& limits() const noexcept { return lim_; }

private:
    RiskReason check(Side side, Price price, Qty qty, Qty releases, const TopRef& ref, const Exposure& x,
                     std::uint64_t now_ns) noexcept {
        ++stats_.checked;
        RiskReason r = evaluate(side, price, qty, releases, ref, x, now_ns);
        if (r == RiskReason::Ok) ++stats_.passed;
        else ++stats_.refused[static_cast<int>(r)];
        return r;
    }

    RiskReason evaluate(Side side, Price price, Qty qty, Qty releases, const TopRef& ref, const Exposure& x,
                        std::uint64_t now_ns) noexcept {
        // 1. kill switch
        if (killed_) return RiskReason::KillSwitch;
        // 2. a reference price exists
        if (!ref.two_sided) return RiskReason::NoReference;
        // 3. size
        if (qty == 0 || qty > lim_.max_order_qty) return RiskReason::OrderSize;
        // 4. price collar, in half-ticks to stay exact: |2*price - mid2| <= 2*collar
        const std::int64_t off2 = std::int64_t(price) * 2 - std::int64_t(ref.mid2);
        if (off2 > 2 * std::int64_t(lim_.collar_ticks) || off2 < -2 * std::int64_t(lim_.collar_ticks))
            return RiskReason::PriceCollar;
        // 5. notional
        if (std::uint64_t(price) * qty > lim_.max_notional) return RiskReason::Notional;
        // 6. open orders (a replace does not add one)
        if (releases == 0 && x.open_orders >= lim_.max_open_orders) return RiskReason::OpenOrders;
        // 7. worst-case position
        if (side == Side::Buy) {
            const std::int64_t worst = x.position + std::int64_t(x.open_buy_qty) - std::int64_t(releases) + std::int64_t(qty);
            if (worst > lim_.max_position) return RiskReason::Position;
        } else {
            const std::int64_t worst = x.position - std::int64_t(x.open_sell_qty) + std::int64_t(releases) - std::int64_t(qty);
            if (worst < -lim_.max_position) return RiskReason::Position;
        }
        // 8. message rate: a one-second window, reset when it rolls over
        if (!count_message(now_ns)) return RiskReason::MessageRate;
        return RiskReason::Ok;
    }

    // Returns false if this message exceeds the per-second limit.
    bool count_message(std::uint64_t now_ns) noexcept {
        if (now_ns - window_start_ >= 1'000'000'000ull) { window_start_ = now_ns; in_window_ = 0; }
        return ++in_window_ <= lim_.max_msgs_per_sec;
    }

    RiskLimits    lim_;
    bool          killed_ = false;
    std::uint64_t window_start_ = 0;
    std::uint32_t in_window_ = 0;
    Stats         stats_;
};

}  // namespace lob
