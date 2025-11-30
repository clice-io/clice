#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Test/Test.h"
#include "Support/JSON.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace {

struct ValueRef {
    std::size_t index;
};

}  // namespace

template <>
struct clice::json::Serde<ValueRef> {
    constexpr inline static bool stateful = true;

    std::vector<int>& encoder;
    std::vector<int>& decoder;

    json::Value serialize(const ValueRef& ref) {
        return json::Value(decoder[ref.index]);
    }

    ValueRef deserialize(const json::Value& value) {
        encoder.emplace_back(*value.getAsInteger());
        return ValueRef{encoder.size() - 1};
    }
};

namespace clice::testing {

namespace {

TEST_SUITE(JSON) {

TEST_CASE(String) {
    json::Value expected = "hello";

    std::string input = "hello";
    ASSERT_EQ(json::serialize(input), expected);
    ASSERT_EQ(json::deserialize<std::string>(expected), input);

    std::string_view input2 = "hello";
    ASSERT_EQ(json::serialize(input2), expected);
    ASSERT_EQ(json::deserialize<std::string_view>(expected), input2);

    llvm::StringRef input3 = "hello";
    ASSERT_EQ(json::serialize(input3), expected);
    ASSERT_EQ(json::deserialize<llvm::StringRef>(expected), input3);

    llvm::SmallString<5> input4 = {"hello"};
    ASSERT_EQ(json::serialize(input4), expected);
    ASSERT_EQ(json::deserialize<llvm::SmallString<5>>(expected), input4);
}

TEST_CASE(MapRange) {
    json::Value expected = json::Object{
        {"1", 2},
        {"3", 4},
        {"5", 6}
    };

    std::map<int, int> input = {
        {1, 2},
        {3, 4},
        {5, 6}
    };

    ASSERT_EQ(json::serialize(input), expected);
    ASSERT_EQ(json::deserialize<decltype(input)>(expected), input);

    std::unordered_map<int, int> input2 = {
        {1, 2},
        {3, 4},
        {5, 6}
    };
    ASSERT_EQ(json::serialize(input2), expected);
    ASSERT_EQ(json::deserialize<decltype(input2)>(expected), input2);

    llvm::DenseMap<int, int> input4 = {
        {1, 2},
        {3, 4},
        {5, 6}
    };
    ASSERT_EQ(json::serialize(input4), expected);
    ASSERT_EQ(json::deserialize<decltype(input4)>(expected), input4);
}

TEST_CASE(SetRange) {
    json::Value expected = {1, 2, 3, 4, 5};

    std::set<int> input = {1, 2, 3, 4, 5};
    ASSERT_EQ(json::deserialize<decltype(input)>(expected), input);

    std::unordered_set<int> input2 = {1, 2, 3, 4, 5};
    ASSERT_EQ(json::deserialize<decltype(input2)>(expected), input2);
}

TEST_CASE(SequenceRange) {
    json::Value expected = {1, 2, 3, 4, 5};

    std::vector<int> input = {1, 2, 3, 4, 5};
    ASSERT_EQ(json::serialize(input), expected);
    ASSERT_EQ(json::deserialize<std::vector<int>>(expected), input);

    llvm::ArrayRef<int> input2 = input;
    ASSERT_EQ(json::serialize(input2), expected);

    llvm::SmallVector<int, 5> input3 = {1, 2, 3, 4, 5};
    ASSERT_EQ(json::serialize(input3), expected);
    ASSERT_EQ((json::deserialize<llvm::SmallVector<int, 5>>(expected)), input3);
}

TEST_CASE(Enum) {
    enum class E { A, B, C };

    json::Value expected = json::Value(1);

    E input = E::B;
    ASSERT_EQ(json::serialize(input), expected);
    ASSERT_EQ(json::deserialize<E>(expected), input);

    struct Color : refl::Enum<Color, false, int> {
        enum Kind {
            Red = 0,
            Green,
            Blue,
            Yellow,
        };

        using Enum::Enum;
    };

    json::Value expected2 = json::Value(2);

    Color input2 = Color::Blue;
    ASSERT_EQ(json::serialize(input2), expected2);
    ASSERT_EQ(json::deserialize<Color>(expected2), input2);
}

TEST_CASE(Struct) {
    struct A {
        int x;
        int y;

        bool operator==(const A& other) const = default;
    };

    json::Value expected = json::Object{
        {"x", 1},
        {"y", 2}
    };

    A input = {1, 2};
    ASSERT_EQ(json::serialize(input), expected);
    ASSERT_EQ(json::deserialize<A>(expected), input);

    struct B {
        A a;
        std::string s;

        bool operator==(const B& other) const = default;
    };

    json::Value expected2 = json::Object{
        {"a", json::Object{{"x", 1}, {"y", 2}}},
        {"s", "hello"                         }
    };

    B input2 = {
        {1, 2},
        "hello",
    };
    ASSERT_EQ(json::serialize(input2), expected2);
    ASSERT_EQ(json::deserialize<B>(expected2), input2);
}

};  // TEST_SUITE(JSON)

}  // namespace

}  // namespace clice::testing
