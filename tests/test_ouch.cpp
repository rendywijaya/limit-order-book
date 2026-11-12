// test_ouch.cpp — OUCH 4.2 layouts, byte for byte.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "lob/ouch.hpp"

using namespace lob;

TEST(Ouch, TokensAreFourteenDigits) {
    auto t = ouch::make_token(42);
    EXPECT_EQ(std::memcmp(t.b, "00000000000042", 14), 0);
    EXPECT_EQ(ouch::token_value(t), 42u);
    EXPECT_TRUE(t == ouch::make_token(42));
    EXPECT_FALSE(t == ouch::make_token(43));
}

// Enter Order, field by field from the specification:
//   'O' | token 14 | side 1 | shares 4 | stock 8 | price 4 | tif 4 | firm 4 | display 1 |
//   capacity 1 | iso 1 | min qty 4 | cross type 1 | customer type 1   = 49 bytes
TEST(Ouch, EnterOrderEncodesToTheSpecLayout) {
    ouch::Enter e{};
    e.token = ouch::make_token(7);
    e.side = 'B'; e.shares = 100;
    wire::put_alpha(e.stock, 8, "AAPL");
    e.price = 1'500'000; e.tif = ouch::kTifDay;
    wire::put_alpha(e.firm, 4, "FIRM");
    e.display = 'Y'; e.capacity = 'P'; e.iso = 'N'; e.min_qty = 0; e.cross_type = 'N'; e.customer = 'N';
    std::uint8_t p[64];
    ASSERT_EQ(ouch::encode(p, e), 49u);
    EXPECT_EQ(p[0], 'O');
    EXPECT_EQ(std::memcmp(p + 1, "00000000000007", 14), 0);
    EXPECT_EQ(p[15], 'B');
    EXPECT_EQ(wire::be32(p + 16), 100u);
    EXPECT_EQ(std::memcmp(p + 20, "AAPL    ", 8), 0);
    EXPECT_EQ(wire::be32(p + 28), 1'500'000u);
    EXPECT_EQ(wire::be32(p + 32), 99998u);
    EXPECT_EQ(std::memcmp(p + 36, "FIRM", 4), 0);
    EXPECT_EQ(p[40], 'Y'); EXPECT_EQ(p[41], 'P'); EXPECT_EQ(p[42], 'N');
    EXPECT_EQ(wire::be32(p + 43), 0u);
    EXPECT_EQ(p[47], 'N'); EXPECT_EQ(p[48], 'N');

    ouch::Enter d{};
    ASSERT_TRUE(ouch::decode({p, 49}, d));
    EXPECT_TRUE(d.token == e.token);
    EXPECT_EQ(d.side, 'B'); EXPECT_EQ(d.shares, 100u); EXPECT_EQ(d.price, 1'500'000u); EXPECT_EQ(d.tif, 99998u);
    EXPECT_EQ(wire::alpha(d.stock, 8), "AAPL");
}

TEST(Ouch, ReplaceAndCancelRoundTrip) {
    std::uint8_t p[64];
    ouch::Replace r{};
    r.token = ouch::make_token(7); r.new_token = ouch::make_token(8);
    r.shares = 200; r.price = 1'510'000; r.tif = ouch::kTifDay; r.display = 'Y'; r.iso = 'N'; r.min_qty = 0;
    ASSERT_EQ(ouch::encode(p, r), 47u);
    EXPECT_EQ(p[0], 'U');
    EXPECT_EQ(std::memcmp(p + 15, "00000000000008", 14), 0);
    EXPECT_EQ(wire::be32(p + 29), 200u);
    EXPECT_EQ(wire::be32(p + 33), 1'510'000u);
    ouch::Replace rd{};
    ASSERT_TRUE(ouch::decode({p, 47}, rd));
    EXPECT_TRUE(rd.new_token == r.new_token); EXPECT_EQ(rd.shares, 200u); EXPECT_EQ(rd.price, 1'510'000u);

    ouch::Cancel c{ouch::make_token(7), 0};
    ASSERT_EQ(ouch::encode(p, c), 19u);
    EXPECT_EQ(p[0], 'X');
    EXPECT_EQ(wire::be32(p + 15), 0u);
    ouch::Cancel cd{};
    ASSERT_TRUE(ouch::decode({p, 19}, cd));
    EXPECT_TRUE(cd.token == c.token);
    EXPECT_FALSE(ouch::decode({p, 18}, cd));  // short
}

TEST(Ouch, OutboundLengthsAndDecoding) {
    EXPECT_EQ(ouch::out_length_of('A'), 66u);
    EXPECT_EQ(ouch::out_length_of('U'), 80u);
    EXPECT_EQ(ouch::out_length_of('C'), 28u);
    EXPECT_EQ(ouch::out_length_of('E'), 40u);
    EXPECT_EQ(ouch::out_length_of('J'), 24u);
    EXPECT_EQ(ouch::out_length_of('S'), 10u);

    std::uint8_t p[96];
    std::uint8_t stock[8];
    wire::put_alpha(stock, 8, "MSFT");
    auto tok = ouch::make_token(99);
    ouch::Out o;

    ASSERT_EQ(ouch::encode_accepted(p, 123, tok, 'S', 300, stock, 4'100'000, ouch::kTifDay, 777, 'L'), 66u);
    EXPECT_EQ(wire::be64(p + 1), 123u);             // timestamp at offset 1
    EXPECT_EQ(std::memcmp(p + 9, tok.b, 14), 0);    // token at 9
    EXPECT_EQ(p[23], 'S');                          // side at 23
    EXPECT_EQ(wire::be32(p + 24), 300u);            // shares at 24
    EXPECT_EQ(wire::be32(p + 36), 4'100'000u);      // price at 36
    EXPECT_EQ(wire::be64(p + 49), 777u);            // order reference at 49
    EXPECT_EQ(p[64], 'L');                          // order state at 64
    ASSERT_TRUE(ouch::decode_out({p, 66}, o));
    EXPECT_EQ(o.type, 'A'); EXPECT_EQ(o.ns, 123u); EXPECT_TRUE(o.token == tok); EXPECT_EQ(o.side, 'S');
    EXPECT_EQ(o.shares, 300u); EXPECT_EQ(o.price, 4'100'000u); EXPECT_EQ(o.order_ref, 777u); EXPECT_EQ(o.state, 'L');

    auto prev = ouch::make_token(98);
    ASSERT_EQ(ouch::encode_replaced(p, 124, tok, 'S', 250, stock, 4'110'000, ouch::kTifDay, 778, 'L', prev), 80u);
    ASSERT_TRUE(ouch::decode_out({p, 80}, o));
    EXPECT_EQ(o.type, 'U'); EXPECT_TRUE(o.prev_token == prev); EXPECT_EQ(o.shares, 250u);

    ASSERT_EQ(ouch::encode_canceled(p, 125, tok, 50, 'U'), 28u);
    ASSERT_TRUE(ouch::decode_out({p, 28}, o));
    EXPECT_EQ(o.type, 'C'); EXPECT_EQ(o.shares, 50u); EXPECT_EQ(o.reason, 'U');

    ASSERT_EQ(ouch::encode_executed(p, 126, tok, 100, 4'100'000, 'A', 555), 40u);
    ASSERT_TRUE(ouch::decode_out({p, 40}, o));
    EXPECT_EQ(o.type, 'E'); EXPECT_EQ(o.shares, 100u); EXPECT_EQ(o.price, 4'100'000u); EXPECT_EQ(o.reason, 'A'); EXPECT_EQ(o.match, 555u);

    ASSERT_EQ(ouch::encode_rejected(p, 127, tok, 'X'), 24u);
    ASSERT_TRUE(ouch::decode_out({p, 24}, o));
    EXPECT_EQ(o.type, 'J'); EXPECT_EQ(o.reason, 'X');

    ASSERT_EQ(ouch::encode_system_event(p, 128, 'S'), 10u);
    ASSERT_TRUE(ouch::decode_out({p, 10}, o));
    EXPECT_EQ(o.type, 'S'); EXPECT_EQ(o.event, 'S');

    EXPECT_FALSE(ouch::decode_out({p, 9}, o));
    p[0] = '?';
    EXPECT_FALSE(ouch::decode_out({p, 66}, o));
}
