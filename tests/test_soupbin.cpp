// test_soupbin.cpp — SoupBinTCP framing, partial reads, login and sequencing.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "lob/soupbin.hpp"

using namespace lob;

TEST(SoupBin, FramesCarryLengthTypeAndPayload) {
    std::uint8_t p[32];
    std::uint8_t body[3] = {'a', 'b', 'c'};
    ASSERT_EQ(soup::frame(p, 'U', {body, 3}), 6u);
    EXPECT_EQ(wire::be16(p), 4u);  // type + 3 payload bytes
    EXPECT_EQ(p[2], 'U');
    EXPECT_EQ(std::memcmp(p + 3, "abc", 3), 0);
    ASSERT_EQ(soup::frame(p, 'R', {}), 3u);  // a heartbeat: length 1, type only
    EXPECT_EQ(wire::be16(p), 1u);
}

TEST(SoupBin, LoginRequestLayout) {
    std::uint8_t p[64];
    ASSERT_EQ(soup::frame_login_request(p, "user", "pass", "SESSION01", 1234), 49u);
    EXPECT_EQ(wire::be16(p), 47u);
    EXPECT_EQ(p[2], 'L');
    EXPECT_EQ(std::memcmp(p + 3, "user  ", 6), 0);
    EXPECT_EQ(std::memcmp(p + 9, "pass      ", 10), 0);
    EXPECT_EQ(std::memcmp(p + 19, "SESSION01 ", 10), 0);
    EXPECT_EQ(std::memcmp(p + 29, "                1234", 20), 0);
}

TEST(SoupBin, FramerReassemblesPacketsSplitAnywhere) {
    // Three packets in one byte string, delivered one byte at a time, then in
    // odd chunks, must produce the same three packets.
    std::vector<std::uint8_t> stream;
    std::uint8_t p[64];
    std::uint8_t a[5] = {1, 2, 3, 4, 5};
    stream.insert(stream.end(), p, p + soup::frame(p, 'S', {a, 5}));
    stream.insert(stream.end(), p, p + soup::frame(p, 'H', {}));
    std::uint8_t b[40]; std::memset(b, 7, 40);
    stream.insert(stream.end(), p, p + soup::frame(p, 'S', {b, 40}));

    for (std::size_t chunk : {1u, 2u, 3u, 7u, 64u}) {
        soup::Framer<256> fr;
        std::vector<std::pair<char, std::size_t>> got;
        for (std::size_t off = 0; off < stream.size(); off += chunk) {
            std::size_t n = std::min(chunk, stream.size() - off);
            ASSERT_TRUE(fr.feed({stream.data() + off, n}, [&](char t, wire::Bytes pl) { got.emplace_back(t, pl.size()); }));
        }
        ASSERT_EQ(got.size(), 3u) << "chunk " << chunk;
        EXPECT_EQ(got[0], std::make_pair('S', std::size_t(5)));
        EXPECT_EQ(got[1], std::make_pair('H', std::size_t(0)));
        EXPECT_EQ(got[2], std::make_pair('S', std::size_t(40)));
        EXPECT_EQ(fr.pending(), 0u);
    }
}

TEST(SoupBin, FramerRejectsAPacketLargerThanItsBuffer) {
    soup::Framer<16> fr;
    std::uint8_t hdr[3] = {0x00, 0x40, 'S'};  // claims 64 bytes
    EXPECT_FALSE(fr.feed({hdr, 3}, [](char, wire::Bytes) {}));
}

TEST(SoupBin, ClientSessionLogsInSequencesAndHeartbeats) {
    std::vector<std::uint8_t> wire_out;
    std::vector<std::pair<std::uint64_t, std::size_t>> data;
    soup::ClientSession cs([&](wire::Bytes b) { wire_out.insert(wire_out.end(), b.begin(), b.end()); },
                           [&](std::uint64_t seq, wire::Bytes pl) { data.emplace_back(seq, pl.size()); });
    EXPECT_EQ(cs.state(), soup::ClientSession::State::Disconnected);
    cs.login("user", "pass", "", 0);
    EXPECT_EQ(cs.state(), soup::ClientSession::State::LoggingIn);
    ASSERT_EQ(wire_out.size(), 49u);
    EXPECT_EQ(wire_out[2], 'L');
    wire_out.clear();

    // The server accepts with session "DAY1" and says the next sequence is 100.
    std::uint8_t p[64];
    cs.on_bytes({p, soup::frame_login_accepted(p, "DAY1", 100)}, 1);
    EXPECT_EQ(cs.state(), soup::ClientSession::State::LoggedIn);
    EXPECT_EQ(cs.session(), "DAY1");
    EXPECT_EQ(cs.next_seq(), 100u);

    // Two sequenced packets arrive, numbered 100 and 101 implicitly.
    std::uint8_t x[4] = {1, 2, 3, 4};
    std::vector<std::uint8_t> two;
    two.insert(two.end(), p, p + soup::frame(p, 'S', {x, 4}));
    two.insert(two.end(), p, p + soup::frame(p, 'S', {x, 2}));
    cs.on_bytes(two, 2);
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0].first, 100u); EXPECT_EQ(data[0].second, 4u);
    EXPECT_EQ(data[1].first, 101u); EXPECT_EQ(data[1].second, 2u);
    EXPECT_EQ(cs.next_seq(), 102u);  // what a reconnect would ask for

    // Sending data resets the heartbeat clock; a quiet second sends one.
    cs.send_data({x, 4}, 1'000'000'000ull);
    ASSERT_EQ(wire_out.size(), 3u + 4u);
    EXPECT_EQ(wire_out[2], 'U');
    wire_out.clear();
    EXPECT_TRUE(cs.tick(1'500'000'000ull));
    EXPECT_EQ(wire_out.size(), 0u);
    EXPECT_TRUE(cs.tick(2'000'000'001ull));
    ASSERT_EQ(wire_out.size(), 3u);
    EXPECT_EQ(wire_out[2], 'R');
    EXPECT_EQ(cs.heartbeats_sent(), 1u);
    // Fifteen silent seconds from the server: the session is considered dead.
    EXPECT_FALSE(cs.tick(2 + 15'000'000'000ull));

    cs.on_bytes({p, soup::frame(p, 'Z', {})}, 3);
    EXPECT_EQ(cs.state(), soup::ClientSession::State::Ended);
}

TEST(SoupBin, LoginRejectedCarriesTheReason) {
    soup::ClientSession cs([](wire::Bytes) {}, [](std::uint64_t, wire::Bytes) {});
    cs.login("u", "p", "", 0);
    std::uint8_t p[8];
    cs.on_bytes({p, soup::frame_login_rejected(p, 'A')}, 1);
    EXPECT_EQ(cs.state(), soup::ClientSession::State::Rejected);
    EXPECT_EQ(cs.reject_reason(), 'A');
}
