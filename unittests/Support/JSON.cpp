#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <vector>

#include "Test/Test.h"
#include "Support/JSON.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"

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

TEST(JSON, String) {
    json::Value expect = "hello";

    std::string input = "hello";
    EXPECT_EQ(json::serialize(input), expect);
    EXPECT_EQ(json::deserialize<std::string>(expect), input);

    std::string_view input2 = "hello";
    EXPECT_EQ(json::serialize(input2), expect);
    EXPECT_EQ(json::deserialize<std::string_view>(expect), input2);

    llvm::StringRef input3 = "hello";
    EXPECT_EQ(json::serialize(input3), expect);
    EXPECT_EQ(json::deserialize<llvm::StringRef>(expect), input3);

    llvm::SmallString<5> input4 = {"hello"};
    EXPECT_EQ(json::serialize(input4), expect);
    EXPECT_EQ(json::deserialize<llvm::SmallString<5>>(expect), input4);
}

TEST(JSON, MapRange) {
    json::Value expect = json::Object{
        {"1", 2},
        {"3", 4},
        {"5", 6}
    };

    std::map<int, int> input = {
        {1, 2},
        {3, 4},
        {5, 6}
    };

    EXPECT_EQ(json::serialize(input), expect);
    EXPECT_EQ(input, json::deserialize<decltype(input)>(expect));

    std::unordered_map<int, int> input2 = {
        {1, 2},
        {3, 4},
        {5, 6}
    };
    EXPECT_EQ(json::serialize(input2), expect);
    EXPECT_EQ(input2, json::deserialize<decltype(input2)>(expect));

    llvm::DenseMap<int, int> input4 = {
        {1, 2},
        {3, 4},
        {5, 6}
    };
    EXPECT_EQ(json::serialize(input4), expect);
    EXPECT_EQ(input4, json::deserialize<decltype(input4)>(expect));
}

TEST(JSON, SetRange) {
    json::Value expect = {1, 2, 3, 4, 5};

    std::set<int> input = {1, 2, 3, 4, 5};
    EXPECT_EQ(input, json::deserialize<decltype(input)>(expect));

    std::unordered_set<int> input2 = {1, 2, 3, 4, 5};
    EXPECT_EQ(input2, json::deserialize<decltype(input2)>(expect));
}

TEST(JSON, SequenceRange) {
    json::Value expect = {1, 2, 3, 4, 5};

    std::vector<int> input = {1, 2, 3, 4, 5};
    EXPECT_EQ(json::serialize(input), expect);
    EXPECT_EQ(json::deserialize<std::vector<int>>(expect), input);

    llvm::ArrayRef<int> input2 = input;
    EXPECT_EQ(json::serialize(input2), expect);

    llvm::SmallVector<int, 5> input3 = {1, 2, 3, 4, 5};
    EXPECT_EQ(json::serialize(input3), expect);
    EXPECT_EQ((json::deserialize<llvm::SmallVector<int, 5>>(expect)), input3);
}

TEST(JSON, Enum) {
    enum class E { A, B, C };

    json::Value expect = json::Value(1);

    E input = E::B;
    EXPECT_EQ(json::serialize(input), expect);
    EXPECT_EQ(json::deserialize<E>(expect), input);

    struct Color : refl::Enum<Color, false, int> {
        enum Kind {
            Red = 0,
            Green,
            Blue,
            Yellow,
        };

        using Enum::Enum;
    };

    json::Value expect2 = json::Value(2);

    Color input2 = Color::Blue;
    EXPECT_EQ(json::serialize(input2), expect2);
    EXPECT_EQ(json::deserialize<Color>(expect2), input2);
}

TEST(JSON, Struct) {
    struct A {
        int x;
        int y;

        bool operator== (const A& other) const = default;
    };

    json::Value expect = json::Object{
        {"x", 1},
        {"y", 2}
    };

    A input = {1, 2};
    EXPECT_EQ(json::serialize(input), expect);
    EXPECT_EQ(json::deserialize<A>(expect), input);

    struct B {
        A a;
        std::string s;

        bool operator== (const B& other) const = default;
    };

    json::Value expect2 = json::Object{
        {"a", json::Object{{"x", 1}, {"y", 2}}},
        {"s", "hello"                         }
    };

    B input2 = {
        {1, 2},
        "hello",
    };
    EXPECT_EQ(json::serialize(input2), expect2);
    EXPECT_EQ(json::deserialize<B>(expect2), input2);
}

}  // namespace

}  // namespace clice::testing
