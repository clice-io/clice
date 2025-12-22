#include "Test/Test.h"
#include "Support/Compare.h"

namespace clice::testing {

namespace {

struct Point {
    int x;
    int y;
};

struct Line {
    Point begin;
    Point end;
};

TEST_SUITE(Compare) {

TEST_CASE(Equal) {
    constexpr Point p1{1, 2};
    constexpr Point p2{1, 2};
    constexpr Point p3{2, 3};

    static_assert(refl::equal(p1, p2));
    static_assert(!refl::equal(p1, p3));

    constexpr Line l1{
        {1, 2},
        {3, 4}
    };
    constexpr Line l2{
        {1, 2},
        {3, 4}
    };
    constexpr Line l3{
        {1, 2},
        {4, 5}
    };

    static_assert(refl::equal(l1, l2));
    static_assert(!refl::equal(l1, l3));
}

TEST_CASE(Less) {
    constexpr Point p1{1, 2};
    constexpr Point p2{2, 3};

    static_assert(refl::less(p1, p2));
    static_assert(!refl::less(p2, p1));

    constexpr Line l1{
        {1, 2},
        {3, 4}
    };
    constexpr Line l2{
        {1, 2},
        {4, 5}
    };

    static_assert(refl::less(l1, l2));
    static_assert(!refl::less(l2, l1));
}

};  // TEST_SUITE(Compare)

}  // namespace

}  // namespace clice::testing
