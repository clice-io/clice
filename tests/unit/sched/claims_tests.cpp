#include "test/test.h"
#include "sched/claims/registry.h"

namespace clice::testing {
namespace {

using namespace worker;

Hash128 hash(std::uint64_t n) {
    return {.low = n, .high = 0};
}

ClaimUnit unit(std::uint64_t key, std::vector<std::uint64_t> elements = {}) {
    ClaimUnit result{.key = hash(key)};
    for(auto element: elements) {
        result.elements.push_back(hash(element));
    }
    return result;
}

ClaimParams request(std::uint64_t attempt,
                    std::vector<ClaimUnit> units,
                    llvm::StringRef fingerprint = "checks") {
    return {.attempt = attempt,
            .file = "tu.cpp",
            .fingerprint = fingerprint.str(),
            .files = {{.path = "h.h", .units = std::move(units)}}};
}

/// The grant rules in isolation: a unit goes to one attempt, an element
/// to the first attempt that brings it, a released grant is offered
/// again, and a key nobody finished is owed to the sweep.
TEST_SUITE(ClaimRegistry) {

ClaimRegistry registry;

std::vector<ClaimRun> runs(const ClaimResult& result) {
    return result.files.front().runs;
}

TEST_CASE(UnitGrantedOnce) {
    auto first = registry.claim(1, Fid{1}, request(1, {unit(10), unit(11)}), {Fid{7}});
    EXPECT_TRUE(runs(first) == std::vector{ClaimRun::Full, ClaimRun::Full});

    // In flight or done, the second asker skips it.
    auto second = registry.claim(2, Fid{2}, request(2, {unit(10), unit(11)}), {Fid{7}});
    EXPECT_TRUE(runs(second) == std::vector{ClaimRun::Skip, ClaimRun::Skip});
    registry.land(1);
    registry.land(2);
    auto third = registry.claim(3, Fid{3}, request(3, {unit(10)}), {Fid{7}});
    EXPECT_TRUE(runs(third) == std::vector{ClaimRun::Skip});
    EXPECT_TRUE(registry.unfinished().empty());
}

TEST_CASE(ReleaseOffersAgain) {
    registry.claim(1, Fid{1}, request(1, {unit(10)}), {Fid{7}});
    registry.release(1);
    auto again = registry.claim(2, Fid{2}, request(2, {unit(10)}), {Fid{7}});
    EXPECT_TRUE(runs(again) == std::vector{ClaimRun::Full});
    registry.land(2);
    EXPECT_TRUE(registry.unfinished().empty());
}

TEST_CASE(NewElementsOnly) {
    registry.claim(1, Fid{1}, request(1, {unit(10, {1, 2})}), {Fid{7}});
    registry.land(1);
    // One new instantiation: the unit is re-run for its elements.
    auto more = registry.claim(2, Fid{2}, request(2, {unit(10, {2, 3})}), {Fid{7}});
    EXPECT_TRUE(runs(more) == std::vector{ClaimRun::Elements});
    registry.land(2);
    auto known = registry.claim(3, Fid{3}, request(3, {unit(10, {1, 3})}), {Fid{7}});
    EXPECT_TRUE(runs(known) == std::vector{ClaimRun::Skip});
    EXPECT_TRUE(registry.unfinished().empty());
}

TEST_CASE(PendingElementsSkipped) {
    registry.claim(1, Fid{1}, request(1, {unit(10, {1})}), {Fid{7}});
    // Element 1 is in flight with attempt 1; only element 2 is new.
    auto second = registry.claim(2, Fid{2}, request(2, {unit(10, {1, 2})}), {Fid{7}});
    EXPECT_TRUE(runs(second) == std::vector{ClaimRun::Elements});
    registry.land(2);

    // Attempt 1 failed: the unit and its element are owed again, and the
    // next asker gets the unit whole.
    registry.release(1);
    auto owed = registry.unfinished();
    ASSERT_TRUE(owed.size() == 1);
    EXPECT_TRUE(owed.front().requesters == (llvm::SmallVector<Fid, 2>{Fid{1}}));
    auto third = registry.claim(3, Fid{1}, request(3, {unit(10, {1})}), {Fid{7}});
    EXPECT_TRUE(runs(third) == std::vector{ClaimRun::Full});
    registry.land(3);
    EXPECT_TRUE(registry.unfinished().empty());
}

TEST_CASE(SkippedUnitStaysOwed) {
    registry.claim(1, Fid{1}, request(1, {unit(10)}), {Fid{7}});
    auto skipped = registry.claim(2, Fid{2}, request(2, {unit(10)}), {Fid{7}});
    EXPECT_TRUE(runs(skipped) == std::vector{ClaimRun::Skip});
    registry.land(2);
    registry.release(1);
    // The borrower completed without the unit; it is still nobody's.
    ASSERT_TRUE(registry.unfinished().size() == 1);
    EXPECT_TRUE(registry.unfinished().front().requesters.front() == Fid{1});
}

TEST_CASE(EqualUnitsInOneFile) {
    // Two declarations written identically in one file are two keys.
    auto first = registry.claim(1, Fid{1}, request(1, {unit(10), unit(10)}), {Fid{7}});
    EXPECT_TRUE(runs(first) == std::vector{ClaimRun::Full, ClaimRun::Full});
    registry.land(1);
    auto second = registry.claim(2, Fid{2}, request(2, {unit(10), unit(10), unit(10)}), {Fid{7}});
    EXPECT_TRUE(runs(second) == std::vector{ClaimRun::Skip, ClaimRun::Skip, ClaimRun::Full});
}

TEST_CASE(OwedElementNamesItsAskers) {
    registry.claim(1, Fid{1}, request(1, {unit(10, {1})}), {Fid{7}});
    registry.land(1);
    registry.claim(2, Fid{2}, request(2, {unit(10, {1, 2})}), {Fid{7}});
    auto third = registry.claim(3, Fid{3}, request(3, {unit(10, {2})}), {Fid{7}});
    EXPECT_TRUE(runs(third) == std::vector{ClaimRun::Skip});
    registry.land(3);
    registry.release(2);

    // Only a TU that materializes element 2 can check it.
    auto owed = registry.unfinished();
    ASSERT_TRUE(owed.size() == 1);
    EXPECT_TRUE(owed.front().requesters == (llvm::SmallVector<Fid, 2>{Fid{2}}));
}

TEST_CASE(Namespaces) {
    auto a = registry.claim(1, Fid{1}, request(1, {unit(10)}, "checks-a"), {Fid{7}});
    auto b = registry.claim(2, Fid{2}, request(2, {unit(10)}, "checks-b"), {Fid{7}});
    auto other_file = registry.claim(3, Fid{3}, request(3, {unit(10)}), {Fid{8}});
    EXPECT_TRUE(runs(a) == std::vector{ClaimRun::Full});
    EXPECT_TRUE(runs(b) == std::vector{ClaimRun::Full});
    EXPECT_TRUE(runs(other_file) == std::vector{ClaimRun::Full});
}

TEST_CASE(UnknownAttemptIgnored) {
    registry.land(42);
    registry.release(42);
    EXPECT_TRUE(registry.unfinished().empty());
}

};  // TEST_SUITE(ClaimRegistry)

}  // namespace
}  // namespace clice::testing
