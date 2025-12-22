#include "Test/Test.h"
#include "Support/Binary.h"
#include "Support/Compare.h"

namespace clice::testing {

namespace {

template <typename Object, typename... Ts>
constexpr inline bool check_sections =
    std::is_same_v<binary::layout_t<Object>, std::tuple<binary::Section<Ts>...>>;

TEST_SUITE(Binary) {

TEST_CASE(String) {
    static_assert(check_sections<std::string, char>);
    {
        std::string s1 = "";
        auto [buffer, proxy] = binary::serialize(s1);
        ASSERT_EQ(s1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(s1[i], proxy[i].value());
        }

        ASSERT_EQ(s1, proxy.as_string());
        std::string s2 = binary::deserialize(proxy);
        ASSERT_EQ(s1, s2);
    }

    {
        std::string s1 = "123";
        auto [buffer, proxy] = binary::serialize(s1);
        ASSERT_EQ(s1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(s1[i], proxy[i].value());
        }

        ASSERT_EQ(s1, proxy.as_string());
        std::string s2 = binary::deserialize(proxy);
        ASSERT_EQ(s1, s2);
    }

    {
        std::string s1 = "11111111111111111111111111111111111111111111111111111111111111111";
        auto [buffer, proxy] = binary::serialize(s1);
        ASSERT_EQ(s1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(s1[i], proxy[i].value());
        }

        ASSERT_EQ(s1, proxy.as_string());
        std::string s2 = binary::deserialize(proxy);
        ASSERT_EQ(s1, s2);
    }
}

TEST_CASE(Array) {
    static_assert(check_sections<std::vector<int>, int>);

    {
        std::vector<int> vec1 = {};
        auto [buffer, proxy] = binary::serialize(vec1);
        ASSERT_EQ(vec1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(vec1[i], proxy[i].value());
        }

        ASSERT_EQ(vec1, proxy.as_array().vec());
        std::vector vec2 = binary::deserialize(proxy);
        ASSERT_EQ(vec1, vec2);
    }

    {
        std::vector vec1 = {1, 2, 3};
        auto [buffer, proxy] = binary::serialize(vec1);
        ASSERT_EQ(vec1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(vec1[i], proxy[i].value());
        }

        ASSERT_EQ(vec1, proxy.as_array().vec());
        std::vector vec2 = binary::deserialize(proxy);
        ASSERT_EQ(vec1, vec2);
    }

    {
        std::vector vec1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        auto [buffer, proxy] = binary::serialize(vec1);
        ASSERT_EQ(vec1.size(), proxy.size());
        for(auto i = 0; i < proxy.size(); i++) {
            ASSERT_EQ(vec1[i], proxy[i].value());
        }

        ASSERT_EQ(vec1, proxy.as_array().vec());
        std::vector vec2 = binary::deserialize(proxy);
        ASSERT_EQ(vec1, vec2);
    }
}

TEST_CASE(StringArray) {
    static_assert(check_sections<std::vector<std::string>, std::string, char>);

    std::vector<std::string> sv = {"1", "22", "333", "444"};
    auto [buffer, proxy] = binary::serialize(sv);
    ASSERT_EQ(sv.size(), proxy.size());

    for(auto i = 0; i < sv.size(); i++) {
        ASSERT_EQ(sv[i], proxy[i].as_string());
    }

    std::vector sv2 = binary::deserialize(proxy);
    ASSERT_EQ(sv, sv2);
}

struct Point {
    uint32_t x;
    uint32_t y;

    bool operator==(const Point& other) const = default;
};

TEST_CASE(Struct) {
    {
        static_assert(binary::is_directly_binarizable_v<Point>);
        static_assert(std::same_as<binary::binarify_t<Point>, Point>);
        static_assert(check_sections<Point>);

        Point p = {1, 2};
        auto [buffer, proxy] = binary::serialize(p);
        ASSERT_EQ(proxy->x, 1U);
        ASSERT_EQ(proxy->y, 2U);
        ASSERT_TRUE(refl::equal(p, proxy.value()));
        auto p2 = binary::deserialize(proxy);
        ASSERT_TRUE(refl::equal(p, p2));
    }

    struct Foo {
        uint32_t age;
        std::string name;
        std::vector<int> scores;
    };

    {
        static_assert(!binary::is_directly_binarizable_v<Foo>);
        static_assert(check_sections<Foo, char, int>);

        Foo foo = {
            0,
            "123",
            {1, 2, 3},
        };

        auto [buffer, proxy] = binary::serialize(foo);
        ASSERT_EQ(proxy.get<"age">().value(), 0U);
        ASSERT_EQ(proxy.get<"name">().as_string(), llvm::StringRef("123"));
        auto vec = std::vector{1, 2, 3};
        ASSERT_EQ(proxy.get<"scores">().as_array().vec(), vec);
        auto foo2 = binary::deserialize(proxy);
        ASSERT_TRUE(refl::equal(foo, foo2));
    };

    struct Points {
        std::vector<Point> points;
    };

    {
        static_assert(!binary::is_directly_binarizable_v<Points>);
        static_assert(check_sections<Points, Point>);

        Points points{
            {
             Point{1, 2},
             Point{3, 4},
             },
        };
        auto [buffer, proxy] = binary::serialize(points);
        auto points2 = proxy.get<"points">();
        ASSERT_TRUE(refl::equal(points2[0].value(), Point{1, 2}));
        ASSERT_TRUE(refl::equal(points2[1].value(), Point{3, 4}));
        auto points3 = binary::deserialize(proxy);
        ASSERT_TRUE(refl::equal(points, points3));
    }
}

struct Node {
    int value;
    std::vector<Node> nodes;
};

TEST_CASE(Recursively) {
    Node node = {
        1,
        {{3},
          {4},
          {
             5,
             {
                 {3},
                 {4},
                 {5},
             },
         }},
    };

    static_assert(!binary::is_directly_binarizable_v<Node>);
    static_assert(check_sections<Node, Node>);

    auto [buffer, proxy] = binary::serialize(node);
    auto node2 = binary::deserialize(proxy);
    ASSERT_TRUE(refl::equal(node, node2));
}

};  // TEST_SUITE(Binary)

}  // namespace

}  // namespace clice::testing
