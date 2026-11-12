// test_spec.cpp — docs/SPEC.md, one test per rule, run against every book.
//
// These are typed tests: the same test body is compiled once per book type, so
// version 1, version 2 and the reference are all held to exactly the same
// event stream for exactly the same input.
#include <gtest/gtest.h>

#include "lob/book_v1.hpp"
#include "lob/book_v2.hpp"
#include "lob/reference_book.hpp"

using namespace lob;
using K = Event::Kind;

namespace {

Event A(OrderId id) { return {K::Accept, id}; }
Event R(OrderId id, RejectReason r) { return {K::Reject, id, 0, 0, 0, 0, std::uint8_t(r)}; }
Event T(OrderId taker, OrderId maker, Price px, Qty q, Qty maker_left) {
    return {K::Trade, taker, maker, px, q, maker_left};
}
Event C(OrderId id, Qty left, CancelReason r) { return {K::Cancel, id, 0, 0, left, 0, std::uint8_t(r)}; }
Event M(OrderId id, Price px, Qty q) { return {K::Modify, id, 0, px, q}; }

constexpr auto B = Side::Buy;
constexpr auto S = Side::Sell;
constexpr auto LMT = OrderType::Limit;
constexpr auto MKT = OrderType::Market;
constexpr auto IOC = OrderType::IOC;
constexpr auto FOK = OrderType::FOK;

}  // namespace

template <class Book>
class Spec : public ::testing::Test {
protected:
    RecordingListener ev;
    BookConfig        cfg{.min_price = 1, .max_price = 1000, .expected_orders = 64};
    Book              book{ev, cfg};

    // Events produced since the last call.
    std::vector<Event> take() { return std::exchange(ev.events, {}); }
    void lim(OrderId id, Side s, Price p, Qty q) { book.add(id, s, LMT, p, q); }
};

using Books = ::testing::Types<BookV1<RecordingListener>, BookV2<RecordingListener>,
                               ReferenceBook<RecordingListener>>;
class BookNames {
public:
    template <class T>
    static std::string GetName(int i) { return i == 0 ? "V1" : i == 1 ? "V2" : "Reference"; }
};
TYPED_TEST_SUITE(Spec, Books, BookNames);

// ---------------------------------------------------------------- resting

TYPED_TEST(Spec, LimitOrderThatDoesNotCrossRests) {
    this->lim(1, B, 100, 10);
    this->lim(2, S, 101, 5);
    EXPECT_EQ(this->take(), (std::vector{A(1), A(2)}));
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{100, 10, 1}}));
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{101, 5, 1}}));
    EXPECT_EQ(this->book.order_count(), 2u);
}

TYPED_TEST(Spec, OrdersAtOnePriceAggregateIntoOneLevel) {
    this->lim(1, B, 100, 10);
    this->lim(2, B, 100, 7);
    this->lim(3, B, 99, 1);
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{100, 17, 2}, {99, 1, 1}}));
    EXPECT_EQ(this->book.depth(B, 1), (std::vector<LevelView>{{100, 17, 2}}));
}

// ---------------------------------------------------------------- priority

TYPED_TEST(Spec, BetterPriceTradesFirst) {
    this->lim(1, S, 102, 1);
    this->lim(2, S, 100, 1);
    this->lim(3, S, 101, 1);
    this->take();
    this->lim(9, B, 102, 3);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 2, 100, 1, 0), T(9, 3, 101, 1, 0), T(9, 1, 102, 1, 0)}));
    EXPECT_TRUE(this->book.depth(S).empty());
    EXPECT_TRUE(this->book.depth(B).empty());
}

TYPED_TEST(Spec, AtOnePriceEarlierOrderTradesFirst) {
    this->lim(1, S, 100, 5);
    this->lim(2, S, 100, 5);
    this->take();
    this->lim(9, B, 100, 7);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 5, 0), T(9, 2, 100, 2, 3)}));
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{100, 3, 1}}));
}

TYPED_TEST(Spec, TradePriceIsTheRestingOrdersPrice) {
    this->lim(1, S, 100, 5);
    this->take();
    this->lim(9, B, 105, 5);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 5, 0)}));
}

TYPED_TEST(Spec, SellTakerHitsHighestBidFirst) {
    this->lim(1, B, 98, 1);
    this->lim(2, B, 100, 1);
    this->lim(3, B, 99, 1);
    this->take();
    this->lim(9, S, 99, 5);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 2, 100, 1, 0), T(9, 3, 99, 1, 0)}));
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{99, 3, 1}}));  // remainder rests
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{98, 1, 1}}));
}

TYPED_TEST(Spec, TakerRemainderRestsAtItsLimit) {
    this->lim(1, S, 100, 4);
    this->take();
    this->lim(9, B, 101, 10);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 4, 0)}));
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{101, 6, 1}}));
}

// ---------------------------------------------------------------- order types

TYPED_TEST(Spec, MarketOrderSweepsAndCancelsRemainder) {
    this->lim(1, S, 100, 2);
    this->lim(2, S, 500, 3);
    this->take();
    this->book.add(9, B, MKT, 0, 10);  // price is ignored for market orders
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 2, 0), T(9, 2, 500, 3, 0),
                                         C(9, 5, CancelReason::Unfilled)}));
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Spec, MarketOrderIntoEmptyBookIsCancelled) {
    this->book.add(9, S, MKT, 0, 10);
    EXPECT_EQ(this->take(), (std::vector{A(9), C(9, 10, CancelReason::Unfilled)}));
}

TYPED_TEST(Spec, IocTradesWhatCrossesAndNeverRests) {
    this->lim(1, S, 100, 2);
    this->lim(2, S, 102, 2);
    this->take();
    this->book.add(9, B, IOC, 101, 10);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 2, 0), C(9, 8, CancelReason::Unfilled)}));
    EXPECT_TRUE(this->book.depth(B).empty());
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{102, 2, 1}}));
}

TYPED_TEST(Spec, IocFullyFilledEmitsNoCancel) {
    this->lim(1, S, 100, 5);
    this->take();
    this->book.add(9, B, IOC, 100, 5);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 5, 0)}));
}

TYPED_TEST(Spec, FokThatCannotFillDoesNothing) {
    this->lim(1, S, 100, 3);
    this->lim(2, S, 101, 3);
    this->take();
    this->book.add(9, B, FOK, 101, 7);
    EXPECT_EQ(this->take(), (std::vector{A(9), C(9, 7, CancelReason::FokNotFilled)}));
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{100, 3, 1}, {101, 3, 1}}));
}

TYPED_TEST(Spec, FokCountsOnlyLiquidityWithinItsLimit) {
    this->lim(1, S, 100, 3);
    this->lim(2, S, 105, 10);
    this->take();
    this->book.add(9, B, FOK, 101, 5);
    EXPECT_EQ(this->take(), (std::vector{A(9), C(9, 5, CancelReason::FokNotFilled)}));
}

TYPED_TEST(Spec, FokThatCanFillFillsEverything) {
    this->lim(1, S, 100, 3);
    this->lim(2, S, 101, 3);
    this->take();
    this->book.add(9, B, FOK, 101, 6);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 1, 100, 3, 0), T(9, 2, 101, 3, 0)}));
    EXPECT_EQ(this->book.order_count(), 0u);
}

// ---------------------------------------------------------------- cancel

TYPED_TEST(Spec, CancelRemovesTheOrder) {
    this->lim(1, B, 100, 10);
    this->lim(2, B, 100, 5);
    this->take();
    this->book.cancel(1);
    EXPECT_EQ(this->take(), (std::vector{C(1, 10, CancelReason::User)}));
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{100, 5, 1}}));
    this->book.cancel(2);
    EXPECT_TRUE(this->book.depth(B).empty());
}

TYPED_TEST(Spec, CancelReportsRemainingAfterPartialFill) {
    this->lim(1, S, 100, 10);
    this->lim(9, B, 100, 4);
    this->take();
    this->book.cancel(1);
    EXPECT_EQ(this->take(), (std::vector{C(1, 6, CancelReason::User)}));
}

TYPED_TEST(Spec, CancelOfUnknownOrDeadOrderIsRejected) {
    this->book.cancel(42);
    this->lim(1, B, 100, 1);
    this->book.cancel(1);
    this->book.cancel(1);
    EXPECT_EQ(this->take(), (std::vector{R(42, RejectReason::UnknownOrder), A(1), C(1, 1, CancelReason::User),
                                         R(1, RejectReason::UnknownOrder)}));
}

// ---------------------------------------------------------------- rejects

TYPED_TEST(Spec, RejectsDuplicateIdOfLiveOrder) {
    this->lim(1, B, 100, 1);
    this->lim(1, S, 200, 1);
    EXPECT_EQ(this->take(), (std::vector{A(1), R(1, RejectReason::DuplicateId)}));
}

TYPED_TEST(Spec, IdOfAFilledOrderCanBeReused) {
    this->lim(1, B, 100, 1);
    this->lim(2, S, 100, 1);
    this->lim(1, B, 99, 1);
    EXPECT_EQ(this->take(), (std::vector{A(1), A(2), T(2, 1, 100, 1, 0), A(1)}));
}

TYPED_TEST(Spec, RejectsZeroQuantityAndPricesOutsideTheWindow) {
    this->lim(1, B, 100, 0);
    this->lim(2, B, 0, 1);
    this->lim(3, B, 1001, 1);
    this->lim(4, B, 1, 1);     // both edges of the window are valid
    this->lim(5, S, 1000, 1);
    EXPECT_EQ(this->take(), (std::vector{R(1, RejectReason::InvalidQty), R(2, RejectReason::InvalidPrice),
                                         R(3, RejectReason::InvalidPrice), A(4), A(5)}));
}

TYPED_TEST(Spec, DuplicateIsCheckedBeforeQuantityBeforePrice) {
    this->lim(1, B, 100, 1);
    this->take();
    this->lim(1, B, 0, 0);
    this->lim(2, B, 0, 0);
    EXPECT_EQ(this->take(), (std::vector{R(1, RejectReason::DuplicateId), R(2, RejectReason::InvalidQty)}));
}

// ---------------------------------------------------------------- modify

TYPED_TEST(Spec, ModifyDownKeepsTimePriority) {
    this->lim(1, S, 100, 10);
    this->lim(2, S, 100, 10);
    this->book.modify(1, 100, 4);
    this->lim(9, B, 100, 5);
    EXPECT_EQ(this->take(), (std::vector{A(1), A(2), M(1, 100, 4), A(9), T(9, 1, 100, 4, 0), T(9, 2, 100, 1, 9)}));
}

TYPED_TEST(Spec, ModifyUpLosesTimePriority) {
    this->lim(1, S, 100, 10);
    this->lim(2, S, 100, 10);
    this->book.modify(1, 100, 11);
    this->take();
    this->lim(9, B, 100, 5);
    EXPECT_EQ(this->take(), (std::vector{A(9), T(9, 2, 100, 5, 5)}));
    EXPECT_EQ(this->book.depth(S), (std::vector<LevelView>{{100, 16, 2}}));
}

TYPED_TEST(Spec, ModifyPriceMovesTheOrderAndCanTrade) {
    this->lim(1, S, 100, 5);
    this->lim(2, B, 95, 8);
    this->take();
    this->book.modify(2, 100, 8);  // now crosses the ask
    EXPECT_EQ(this->take(), (std::vector{M(2, 100, 8), T(2, 1, 100, 5, 0)}));
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{100, 3, 1}}));
    EXPECT_TRUE(this->book.depth(S).empty());
}

TYPED_TEST(Spec, ModifyRejects) {
    this->lim(1, B, 100, 5);
    this->take();
    this->book.modify(7, 100, 5);
    this->book.modify(1, 100, 0);
    this->book.modify(1, 1001, 5);
    EXPECT_EQ(this->take(), (std::vector{R(7, RejectReason::UnknownOrder), R(1, RejectReason::InvalidQty),
                                         R(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->book.depth(B), (std::vector<LevelView>{{100, 5, 1}}));  // untouched
}

// ---------------------------------------------------------------- submit()

TYPED_TEST(Spec, SubmitDispatchesCommands) {
    this->book.submit({MsgType::New, B, LMT, 1, 100, 5});
    this->book.submit({MsgType::Modify, B, LMT, 1, 101, 5});
    this->book.submit({MsgType::Cancel, B, LMT, 1, 0, 0});
    EXPECT_EQ(this->take(), (std::vector{A(1), M(1, 101, 5), C(1, 5, CancelReason::User)}));
}
