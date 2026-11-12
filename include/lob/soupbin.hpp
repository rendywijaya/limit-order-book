// soupbin.hpp — SoupBinTCP 3.0: the session that carries OUCH.
//
// A TCP byte stream has no message boundaries and no memory. SoupBinTCP adds
// both: every packet is [length: 2 bytes big-endian][type: 1][payload], where
// length counts the type byte and the payload; and every message the server
// sends as Sequenced Data has an implicit sequence number, starting from the
// number in the Login Accepted packet. A client that reconnects logs in with
// the next sequence number it expects and the server replays from there, so a
// dropped connection loses nothing.
//
//   client -> server: Login Request 'L', Unsequenced Data 'U' (an OUCH order),
//                     Client Heartbeat 'R', Logout Request 'O', Debug '+'
//   server -> client: Login Accepted 'A', Login Rejected 'J', Sequenced Data 'S'
//                     (an OUCH answer), Server Heartbeat 'H', End of Session 'Z'
//
// Heartbeats go each way every second when nothing else does; 15 silent
// seconds means the other side is gone.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

#include "lob/wire.hpp"

namespace lob::soup {

enum Type : char {
    Debug = '+', LoginAccepted = 'A', LoginRejected = 'J', SequencedData = 'S', ServerHeartbeat = 'H',
    EndOfSession = 'Z', LoginRequest = 'L', UnsequencedData = 'U', ClientHeartbeat = 'R', LogoutRequest = 'O',
};

// ---- framing -------------------------------------------------------------------

// Writes a packet: 2-byte length, type, payload. Returns the total bytes written.
inline std::size_t frame(std::uint8_t* p, char type, wire::Bytes payload) noexcept {
    wire::put16(p, std::uint16_t(1 + payload.size()));
    p[2] = std::uint8_t(type);
    if (!payload.empty()) std::memcpy(p + 3, payload.data(), payload.size());
    return 3 + payload.size();
}

inline std::size_t frame_login_request(std::uint8_t* p, std::string_view user, std::string_view password,
                                       std::string_view session, std::uint64_t next_seq) noexcept {
    std::uint8_t body[46];
    wire::put_alpha(body, 6, user);
    wire::put_alpha(body + 6, 10, password);
    wire::put_alpha(body + 16, 10, session);
    wire::put_ascii_num(body + 26, 20, next_seq);
    return frame(p, LoginRequest, {body, 46});
}
inline std::size_t frame_login_accepted(std::uint8_t* p, std::string_view session, std::uint64_t seq) noexcept {
    std::uint8_t body[30];
    wire::put_alpha(body, 10, session);
    wire::put_ascii_num(body + 10, 20, seq);
    return frame(p, LoginAccepted, {body, 30});
}
inline std::size_t frame_login_rejected(std::uint8_t* p, char reason) noexcept {
    std::uint8_t body[1] = {std::uint8_t(reason)};  // 'A' not authorized, 'S' session not available
    return frame(p, LoginRejected, {body, 1});
}

// Pulls complete packets out of a byte stream. Feed it whatever recv() gave
// you; it calls on_packet(type, payload) for each whole packet and keeps the
// tail of a partial one for next time. Fixed buffer, no allocation.
template <std::size_t Cap = 65536>
class Framer {
public:
    using OnPacket = std::function<void(char type, wire::Bytes payload)>;

    // Returns false if the stream is corrupt (a packet longer than the buffer).
    bool feed(wire::Bytes in, const OnPacket& on_packet) {
        std::size_t off = 0;
        while (off < in.size()) {
            std::size_t take = in.size() - off;
            if (take > Cap - len_) take = Cap - len_;
            std::memcpy(buf_ + len_, in.data() + off, take);
            len_ += take;
            off += take;
            std::size_t pos = 0;
            while (len_ - pos >= 2) {
                std::size_t n = wire::be16(buf_ + pos);
                if (n == 0 || n + 2 > Cap) return false;
                if (len_ - pos < n + 2) break;
                on_packet(char(buf_[pos + 2]), wire::Bytes{buf_ + pos + 3, n - 1});
                pos += n + 2;
            }
            if (pos) { std::memmove(buf_, buf_ + pos, len_ - pos); len_ -= pos; }
            if (len_ == Cap) return false;  // a partial packet filled the buffer
        }
        return true;
    }
    std::size_t pending() const noexcept { return len_; }

private:
    std::uint8_t buf_[Cap];
    std::size_t  len_ = 0;
};

// ---- the client session ---------------------------------------------------------
//
// Tracks login state and the sequence number of the next Sequenced Data
// packet expected, which is what a reconnect sends in its Login Request.
// Transport is abstracted: the owner gives it a sink for outgoing bytes and
// feeds it incoming bytes. Time is abstracted too (nanoseconds from the
// caller), so tests and replays are deterministic.
class ClientSession {
public:
    enum class State : std::uint8_t { Disconnected, LoggingIn, LoggedIn, Rejected, Ended };
    using Send = std::function<void(wire::Bytes)>;
    using OnData = std::function<void(std::uint64_t seq, wire::Bytes ouch_payload)>;

    static constexpr std::uint64_t kHeartbeatNs = 1'000'000'000ull;
    static constexpr std::uint64_t kTimeoutNs = 15'000'000'000ull;

    ClientSession(Send send, OnData on_data) : send_(std::move(send)), on_data_(std::move(on_data)) {}

    void login(std::string_view user, std::string_view password, std::string_view session, std::uint64_t now_ns) {
        std::uint8_t p[64];
        std::size_t n = frame_login_request(p, user, password, session, next_seq_);
        state_ = State::LoggingIn;
        last_sent_ = last_recv_ = now_ns;
        send_({p, n});
    }

    // Send an OUCH message as Unsequenced Data.
    void send_data(wire::Bytes ouch, std::uint64_t now_ns) {
        std::uint8_t p[128];
        std::size_t n = frame(p, UnsequencedData, ouch);
        last_sent_ = now_ns;
        ++sent_;
        send_({p, n});
    }

    // Bytes from the socket.
    void on_bytes(wire::Bytes in, std::uint64_t now_ns) {
        last_recv_ = now_ns;
        framer_.feed(in, [&](char type, wire::Bytes payload) { on_packet(type, payload); });
    }

    // Call periodically: sends a heartbeat if a second has passed since the
    // last send; returns false if the server has been silent for 15 s.
    bool tick(std::uint64_t now_ns) {
        if (state_ != State::LoggedIn) return true;
        if (now_ns - last_sent_ >= kHeartbeatNs) {
            std::uint8_t p[3];
            send_({p, frame(p, ClientHeartbeat, {})});
            last_sent_ = now_ns;
            ++heartbeats_sent_;
        }
        return now_ns - last_recv_ < kTimeoutNs;
    }

    void logout() {
        std::uint8_t p[3];
        send_({p, frame(p, LogoutRequest, {})});
        state_ = State::Disconnected;
    }

    State         state() const noexcept { return state_; }
    std::uint64_t next_seq() const noexcept { return next_seq_; }
    std::string_view session() const noexcept { return wire::alpha(session_, 10); }
    std::uint64_t sent() const noexcept { return sent_; }
    std::uint64_t received() const noexcept { return received_; }
    std::uint64_t heartbeats_sent() const noexcept { return heartbeats_sent_; }
    char          reject_reason() const noexcept { return reject_; }

private:
    void on_packet(char type, wire::Bytes payload) {
        switch (type) {
            case LoginAccepted:
                if (payload.size() >= 30) {
                    std::memcpy(session_, payload.data(), 10);
                    next_seq_ = wire::ascii_num(payload.data() + 10, 20);
                }
                state_ = State::LoggedIn;
                break;
            case LoginRejected:
                reject_ = payload.empty() ? '?' : char(payload[0]);
                state_ = State::Rejected;
                break;
            case SequencedData:
                ++received_;
                on_data_(next_seq_++, payload);
                break;
            case ServerHeartbeat: break;
            case EndOfSession: state_ = State::Ended; break;
            case Debug: break;
            default: break;
        }
    }

    Send          send_;
    OnData        on_data_;
    Framer<>      framer_;
    State         state_ = State::Disconnected;
    std::uint8_t  session_[10] = {};
    std::uint64_t next_seq_ = 1;
    std::uint64_t last_sent_ = 0, last_recv_ = 0;
    std::uint64_t sent_ = 0, received_ = 0, heartbeats_sent_ = 0;
    char          reject_ = 0;
};

}  // namespace lob::soup
