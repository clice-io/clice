#include "test/test.h"
#include "support/lru_map.h"

namespace clice::testing {
namespace {

TEST_SUITE(LRUMap) {

TEST_CASE(BasicInsertFind) {
    LRUMap<int, std::string> map(3);
    map.insert(1, "one");
    map.insert(2, "two");
    map.insert(3, "three");

    ASSERT_EQ(map.size(), 3U);
    ASSERT_EQ(*map.find(1), "one");
    ASSERT_EQ(*map.find(2), "two");
    ASSERT_EQ(*map.find(3), "three");
    ASSERT_EQ(map.find(4), nullptr);
}

TEST_CASE(EvictionOrder) {
    LRUMap<int, std::string> map(2);
    map.insert(1, "one");
    map.insert(2, "two");

    // Inserting a third evicts the LRU (key=1).
    map.insert(3, "three");
    ASSERT_EQ(map.size(), 2U);
    ASSERT_EQ(map.find(1), nullptr);
    ASSERT_NE(map.find(2), nullptr);
    ASSERT_NE(map.find(3), nullptr);
}

TEST_CASE(TouchPromotes) {
    LRUMap<int, std::string> map(2);
    map.insert(1, "one");
    map.insert(2, "two");

    // Touch key=1, making key=2 the LRU.
    map.find(1);

    map.insert(3, "three");
    ASSERT_EQ(map.find(2), nullptr);  // key=2 evicted
    ASSERT_NE(map.find(1), nullptr);  // key=1 survived
    ASSERT_NE(map.find(3), nullptr);
}

TEST_CASE(EvictionCallback) {
    LRUMap<int, std::string> map(2);
    std::vector<int> evicted_keys;
    map.on_evict = [&](int key, std::string&) {
        evicted_keys.push_back(key);
    };

    map.insert(1, "one");
    map.insert(2, "two");
    map.insert(3, "three");

    ASSERT_EQ(evicted_keys.size(), 1U);
    ASSERT_EQ(evicted_keys[0], 1);
}

TEST_CASE(ExplicitEraseNoCallback) {
    LRUMap<int, std::string> map(3);
    bool called = false;
    map.on_evict = [&](int, std::string&) {
        called = true;
    };

    map.insert(1, "one");
    ASSERT_TRUE(map.erase(1));
    ASSERT_FALSE(called);
    ASSERT_EQ(map.size(), 0U);
    ASSERT_FALSE(map.erase(1));
}

TEST_CASE(TryEmplace) {
    LRUMap<int, std::string> map(3);
    auto [ref1, inserted1] = map.try_emplace(1, "one");
    ASSERT_TRUE(inserted1);
    ASSERT_EQ(ref1, "one");

    auto [ref2, inserted2] = map.try_emplace(1, "replaced");
    ASSERT_FALSE(inserted2);
    ASSERT_EQ(ref2, "one");  // not replaced
}

TEST_CASE(OperatorBracket) {
    LRUMap<int, std::string> map(3);
    map[1] = "one";
    map[2] = "two";
    ASSERT_EQ(map[1], "one");
    ASSERT_EQ(map.size(), 2U);
}

TEST_CASE(SetCapacityShrinks) {
    LRUMap<int, std::string> map(5);
    for(int i = 0; i < 5; ++i)
        map.insert(i, std::to_string(i));
    ASSERT_EQ(map.size(), 5U);

    std::vector<int> evicted;
    map.on_evict = [&](int key, std::string&) {
        evicted.push_back(key);
    };

    map.set_capacity(3);
    ASSERT_EQ(map.size(), 3U);
    // Keys 0 and 1 were LRU and should be evicted.
    ASSERT_EQ(evicted.size(), 2U);
    ASSERT_EQ(evicted[0], 0);
    ASSERT_EQ(evicted[1], 1);
}

TEST_CASE(ZeroCapacityUnlimited) {
    // capacity=0 means unlimited (no eviction).
    LRUMap<int, std::string> map(0);
    for(int i = 0; i < 100; ++i)
        map.insert(i, std::to_string(i));
    ASSERT_EQ(map.size(), 100U);
}

TEST_CASE(Peek) {
    LRUMap<int, std::string> map(2);
    map.insert(1, "one");
    map.insert(2, "two");

    // peek does NOT promote.
    auto* val = map.peek(1);
    ASSERT_NE(val, nullptr);
    ASSERT_EQ(*val, "one");

    // key=1 is still LRU since peek didn't promote.
    map.insert(3, "three");
    ASSERT_EQ(map.find(1), nullptr);  // evicted
    ASSERT_NE(map.find(2), nullptr);
}

TEST_CASE(Iteration) {
    LRUMap<int, std::string> map(3);
    map.insert(1, "one");
    map.insert(2, "two");
    map.insert(3, "three");

    // Iteration order: most-recent first → 3, 2, 1.
    std::vector<int> keys;
    for(auto& [k, v]: map)
        keys.push_back(k);
    ASSERT_EQ(keys.size(), 3U);
    ASSERT_EQ(keys[0], 3);
    ASSERT_EQ(keys[1], 2);
    ASSERT_EQ(keys[2], 1);
}

};  // TEST_SUITE(LRUMap)

}  // namespace
}  // namespace clice::testing
