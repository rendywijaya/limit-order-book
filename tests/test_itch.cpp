// test_itch.cpp — ITCH 5.0 decoding, byte for byte.
//
// The first tests build messages by hand from the specification's field
// table, so a wrong offset in the decoder fails against the spec and not
// against the encoder that shares its assumptions. The later tests round-trip
// through the encoders used by the synthetic capture.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "lob/itch.hpp"

using namespace lob;

namespace {
std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int x : v) out.push_back(std::uint8_t(x));
    return out;
}
}  // namespace

TEST(Itch, LengthTableMatchesTheSpecification) {
    EXPECT_EQ(itch::length_of('S'), 12u);
    EXPECT_EQ(itch::length_of('R'), 39u);
    EXPECT_EQ(itch::length_of('A'), 36u);
    EXPECT_EQ(itch::length_of('F'), 40u);
    EXPECT_EQ(itch::length_of('E'), 31u);
    EXPECT_EQ(itch::length_of('C'), 36u);
    EXPECT_EQ(itch::length_of('X'), 23u);
    EXPECT_EQ(itch::length_of('D'), 19u);
    EXPECT_EQ(itch::length_of('U'), 35u);
    EXPECT_EQ(itch::length_of('P'), 44u);
    EXPECT_EQ(itch::length_of('Q'), 40u);
    EXPECT_EQ(itch::length_of('B'), 19u);
    EXPECT_EQ(itch::length_of('?'), 0u);
}

// Add Order, assembled by hand from the spec:
//   type 'A' | locate 2 | tracking 2 | timestamp 6 | order ref 8 | side 1 | shares 4 | stock 8 | price 4
TEST(Itch, DecodesAddOrderFromHandBuiltBytes) {
    auto b = bytes({'A',
                    0x00, 0x2A,                          // locate 42
                    0x00, 0x07,                          // tracking 7
                    0x00, 0x00, 0x00, 0x05, 0xF5, 0xE1,  // timestamp 390625 ns
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,  // ref 65538
                    'B',
                    0x00, 0x00, 0x03, 0xE8,              // 1000 shares
                    'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',
                    0x00, 0x12, 0xD6, 0x87});            // 1234567 = $123.4567
    ASSERT_EQ(b.size(), 36u);
    itch::Msg m;
    ASSERT_TRUE(itch::decode(b, m));
    EXPECT_EQ(m.type, 'A');
    EXPECT_EQ(m.locate, 42);
    EXPECT_EQ(m.tracking, 7);
    EXPECT_EQ(m.ns, 390625u);
    EXPECT_EQ(m.ref, 65538u);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 1000u);
    EXPECT_EQ(wire::alpha(m.stock, 8), "AAPL");
    EXPECT_EQ(m.price, 1234567u);
}

TEST(Itch, DecodesExecutedAndDeleteAndReplace) {
    auto e = bytes({'E', 0, 1, 0, 0, 0, 0, 0, 0, 0, 9,
                    0, 0, 0, 0, 0, 0, 0, 5,              // ref 5
                    0, 0, 0, 200,                        // 200 executed
                    0, 0, 0, 0, 0, 0, 0x30, 0x39});      // match 12345
    itch::Msg m;
    ASSERT_TRUE(itch::decode(e, m));
    EXPECT_EQ(m.type, 'E'); EXPECT_EQ(m.ref, 5u); EXPECT_EQ(m.shares, 200u); EXPECT_EQ(m.match, 12345u);

    auto d = bytes({'D', 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 77});
    ASSERT_TRUE(itch::decode(d, m));
    EXPECT_EQ(m.type, 'D'); EXPECT_EQ(m.ref, 77u);

    auto u = bytes({'U', 0, 1, 0, 0, 0, 0, 0, 0, 0, 9,
                    0, 0, 0, 0, 0, 0, 0, 77,             // original ref 77
                    0, 0, 0, 0, 0, 0, 0, 78,             // new ref 78
                    0, 0, 0, 50,                         // 50 shares
                    0, 0x0F, 0x42, 0x40});               // 1000000 = $100.0000
    ASSERT_TRUE(itch::decode(u, m));
    EXPECT_EQ(m.type, 'U'); EXPECT_EQ(m.ref, 77u); EXPECT_EQ(m.new_ref, 78u);
    EXPECT_EQ(m.shares, 50u); EXPECT_EQ(m.price, 1000000u);
}

TEST(Itch, RejectsShortBuffersAndUnknownTypes) {
    itch::Msg m;
    auto a = bytes({'A', 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 5});  // 19 of 36 bytes
    EXPECT_FALSE(itch::decode(a, m));
    auto z = bytes({'z', 0, 1, 0, 0, 0, 0, 0, 0, 0, 9});
    EXPECT_FALSE(itch::decode(z, m));
    EXPECT_FALSE(itch::decode({}, m));
}

TEST(Itch, EncodersRoundTripThroughTheDecoder) {
    std::uint8_t buf[64];
    itch::Msg m;

    ASSERT_EQ(itch::encode_add(buf, 3, 123456789012ull, 900, 'S', 300, "MSFT", 4125000), 36u);
    ASSERT_TRUE(itch::decode({buf, 36}, m));
    EXPECT_EQ(m.type, 'A'); EXPECT_EQ(m.locate, 3); EXPECT_EQ(m.ns, 123456789012ull);
    EXPECT_EQ(m.ref, 900u); EXPECT_EQ(m.side, 'S'); EXPECT_EQ(m.shares, 300u);
    EXPECT_EQ(wire::alpha(m.stock, 8), "MSFT"); EXPECT_EQ(m.price, 4125000u);

    ASSERT_EQ(itch::encode_executed(buf, 3, 1, 900, 100, 55), 31u);
    ASSERT_TRUE(itch::decode({buf, 31}, m));
    EXPECT_EQ(m.type, 'E'); EXPECT_EQ(m.shares, 100u); EXPECT_EQ(m.match, 55u);

    ASSERT_EQ(itch::encode_cancel(buf, 3, 1, 900, 50), 23u);
    ASSERT_TRUE(itch::decode({buf, 23}, m));
    EXPECT_EQ(m.type, 'X'); EXPECT_EQ(m.shares, 50u);

    ASSERT_EQ(itch::encode_delete(buf, 3, 1, 900), 19u);
    ASSERT_TRUE(itch::decode({buf, 19}, m));
    EXPECT_EQ(m.type, 'D'); EXPECT_EQ(m.ref, 900u);

    ASSERT_EQ(itch::encode_replace(buf, 3, 1, 900, 901, 250, 4130000), 35u);
    ASSERT_TRUE(itch::decode({buf, 35}, m));
    EXPECT_EQ(m.type, 'U'); EXPECT_EQ(m.new_ref, 901u); EXPECT_EQ(m.shares, 250u); EXPECT_EQ(m.price, 4130000u);

    ASSERT_EQ(itch::encode_trade(buf, 3, 1, 'B', 100, "MSFT", 4127500, 56), 44u);
    ASSERT_TRUE(itch::decode({buf, 44}, m));
    EXPECT_EQ(m.type, 'P'); EXPECT_EQ(m.side, 'B'); EXPECT_EQ(m.price, 4127500u); EXPECT_EQ(m.match, 56u);

    ASSERT_EQ(itch::encode_stock_directory(buf, 3, 1, "MSFT"), 39u);
    ASSERT_TRUE(itch::decode({buf, 39}, m));
    EXPECT_EQ(m.type, 'R'); EXPECT_EQ(wire::alpha(m.stock, 8), "MSFT"); EXPECT_EQ(m.shares, 100u);

    ASSERT_EQ(itch::encode_system_event(buf, 1, 'Q'), 12u);
    ASSERT_TRUE(itch::decode({buf, 12}, m));
    EXPECT_EQ(m.type, 'S'); EXPECT_EQ(m.event, 'Q');
}

TEST(Wire, BigEndianAndAlphaHelpers) {
    std::uint8_t b[8];
    wire::put48(b, 0x0102030405ull);
    EXPECT_EQ(wire::be48(b), 0x0102030405ull);
    EXPECT_EQ(b[0], 0); EXPECT_EQ(b[1], 1); EXPECT_EQ(b[5], 5);
    wire::put64(b, 0x0102030405060708ull);
    EXPECT_EQ(wire::be64(b), 0x0102030405060708ull);
    wire::put_alpha(b, 8, "ABC");
    EXPECT_EQ(std::memcmp(b, "ABC     ", 8), 0);
    EXPECT_EQ(wire::alpha(b, 8), "ABC");
    EXPECT_EQ(wire::symbol_key("ABC"), wire::symbol_key(b));
    wire::put_ascii_num(b, 8, 12345);
    EXPECT_EQ(std::memcmp(b, "   12345", 8), 0);
    EXPECT_EQ(wire::ascii_num(b, 8), 12345u);
}
