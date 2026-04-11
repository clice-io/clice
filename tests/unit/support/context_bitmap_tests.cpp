#include "test/test.h"
#include "support/bitmap.h"

namespace clice::testing {
namespace {

TEST_SUITE(ContextBitmap) {

TEST_CASE(EmptyByDefault) {
    ContextBitmap bm;
    EXPECT_TRUE(bm.is_empty());
}

TEST_CASE(AddInline) {
    ContextBitmap bm;
    bm.add(0);
    EXPECT_FALSE(bm.is_empty());
    bm.add(62);
    EXPECT_FALSE(bm.is_empty());

    auto r = bm.to_roaring();
    EXPECT_TRUE(r.contains(0));
    EXPECT_TRUE(r.contains(62));
    EXPECT_FALSE(r.contains(1));
    EXPECT_EQ(r.cardinality(), 2U);
}

TEST_CASE(RemoveInline) {
    ContextBitmap bm;
    bm.add(5);
    bm.add(10);
    bm.remove(5);

    auto r = bm.to_roaring();
    EXPECT_FALSE(r.contains(5));
    EXPECT_TRUE(r.contains(10));
    EXPECT_EQ(r.cardinality(), 1U);

    bm.remove(10);
    EXPECT_TRUE(bm.is_empty());
}

TEST_CASE(UpgradeToHeap) {
    ContextBitmap bm;
    bm.add(5);
    bm.add(63);

    EXPECT_FALSE(bm.is_empty());
    auto r = bm.to_roaring();
    EXPECT_TRUE(r.contains(5));
    EXPECT_TRUE(r.contains(63));
    EXPECT_EQ(r.cardinality(), 2U);
}

TEST_CASE(HeapAddRemove) {
    ContextBitmap bm;
    bm.add(100);
    bm.add(200);
    bm.remove(100);

    auto r = bm.to_roaring();
    EXPECT_FALSE(r.contains(100));
    EXPECT_TRUE(r.contains(200));
}

TEST_CASE(AnyNotInBothInline) {
    ContextBitmap a, b;
    a.add(1);
    a.add(2);
    b.add(1);
    b.add(2);
    EXPECT_FALSE(a.any_not_in(b));

    a.add(3);
    EXPECT_TRUE(a.any_not_in(b));
}

TEST_CASE(AnyNotInInlineVsHeap) {
    ContextBitmap a, b;
    a.add(5);
    b.add(5);
    b.add(100);
    EXPECT_FALSE(a.any_not_in(b));

    a.add(10);
    EXPECT_TRUE(a.any_not_in(b));
}

TEST_CASE(AnyNotInHeapVsInline) {
    ContextBitmap a, b;
    a.add(100);
    b.add(5);
    // 100 >= 63 not in inline b
    EXPECT_TRUE(a.any_not_in(b));

    ContextBitmap c, d;
    c.add(5);
    c.add(100);
    d.add(5);
    // c has 100 which d doesn't have
    EXPECT_TRUE(c.any_not_in(d));
}

TEST_CASE(AnyNotInBothHeap) {
    ContextBitmap a, b;
    a.add(100);
    a.add(200);
    b.add(100);
    b.add(200);
    EXPECT_FALSE(a.any_not_in(b));

    a.add(300);
    EXPECT_TRUE(a.any_not_in(b));
}

TEST_CASE(MoveConstruct) {
    ContextBitmap a;
    a.add(5);
    a.add(10);
    ContextBitmap b(std::move(a));

    EXPECT_TRUE(a.is_empty());
    EXPECT_FALSE(b.is_empty());
    auto r = b.to_roaring();
    EXPECT_TRUE(r.contains(5));
    EXPECT_TRUE(r.contains(10));
}

TEST_CASE(MoveConstructHeap) {
    ContextBitmap a;
    a.add(100);
    ContextBitmap b(std::move(a));

    EXPECT_TRUE(a.is_empty());
    auto r = b.to_roaring();
    EXPECT_TRUE(r.contains(100));
}

TEST_CASE(MoveAssign) {
    ContextBitmap a, b;
    a.add(3);
    b.add(7);
    b = std::move(a);

    EXPECT_TRUE(a.is_empty());
    auto r = b.to_roaring();
    EXPECT_TRUE(r.contains(3));
    EXPECT_FALSE(r.contains(7));
}

TEST_CASE(FromRoaringInline) {
    roaring::Roaring r;
    r.add(0);
    r.add(30);
    r.add(62);
    auto bm = ContextBitmap::from_roaring(r);

    EXPECT_FALSE(bm.is_empty());
    auto out = bm.to_roaring();
    EXPECT_EQ(out.cardinality(), 3U);
    EXPECT_TRUE(out.contains(0));
    EXPECT_TRUE(out.contains(30));
    EXPECT_TRUE(out.contains(62));
}

TEST_CASE(FromRoaringHeap) {
    roaring::Roaring r;
    r.add(5);
    r.add(100);
    auto bm = ContextBitmap::from_roaring(r);

    auto out = bm.to_roaring();
    EXPECT_EQ(out.cardinality(), 2U);
    EXPECT_TRUE(out.contains(5));
    EXPECT_TRUE(out.contains(100));
}

TEST_CASE(FromRoaringEmpty) {
    roaring::Roaring r;
    auto bm = ContextBitmap::from_roaring(r);
    EXPECT_TRUE(bm.is_empty());
}

TEST_CASE(Equality) {
    ContextBitmap a, b;
    a.add(1);
    a.add(2);
    b.add(1);
    b.add(2);
    EXPECT_TRUE(a == b);

    b.add(3);
    EXPECT_FALSE(a == b);
}

TEST_CASE(BoundaryId62) {
    ContextBitmap bm;
    bm.add(62);
    auto r = bm.to_roaring();
    EXPECT_TRUE(r.contains(62));
    EXPECT_EQ(r.cardinality(), 1U);
}

TEST_CASE(BoundaryId63) {
    ContextBitmap bm;
    bm.add(63);
    auto r = bm.to_roaring();
    EXPECT_TRUE(r.contains(63));
    EXPECT_EQ(r.cardinality(), 1U);
}

TEST_CASE(MoveAssignHeap) {
    ContextBitmap a, b;
    a.add(100);
    a.add(200);
    b.add(300);
    b.add(400);
    b = std::move(a);

    EXPECT_TRUE(a.is_empty());
    auto r = b.to_roaring();
    EXPECT_TRUE(r.contains(100));
    EXPECT_TRUE(r.contains(200));
    EXPECT_FALSE(r.contains(300));
    EXPECT_FALSE(r.contains(400));
}

TEST_CASE(EqualityHeap) {
    ContextBitmap a, b;
    a.add(100);
    a.add(200);
    b.add(100);
    b.add(200);
    EXPECT_TRUE(a == b);

    b.add(300);
    EXPECT_FALSE(a == b);
}

TEST_CASE(AddSmallAfterUpgrade) {
    ContextBitmap bm;
    bm.add(100);
    bm.add(5);

    auto r = bm.to_roaring();
    EXPECT_TRUE(r.contains(100));
    EXPECT_TRUE(r.contains(5));
    EXPECT_EQ(r.cardinality(), 2U);
}

};  // TEST_SUITE(ContextBitmap)

}  // namespace
}  // namespace clice::testing
