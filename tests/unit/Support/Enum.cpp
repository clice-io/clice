#include "Test/Test.h"
#include "Support/Enum.h"

namespace clice::testing {

namespace {

struct Color : refl::Enum<Color> {
    enum Kind : uint8_t {
        Red,
        Green,
        Blue,
        Yellow,
        Invalid,
    };

    using Enum::Enum;

    constexpr inline static auto InvalidEnum = Invalid;
};

struct StringEnum : refl::Enum<StringEnum, false, std::string_view> {
    using Enum::Enum;

    constexpr inline static std::string_view A = "A";
    constexpr inline static std::string_view B = "B";
    constexpr inline static std::string_view C = "C";
    constexpr inline static std::string_view D = "D";

    constexpr inline static std::array All = {A, B, C, D};
};

TEST_SUITE(Enum) {

TEST_CASE(EnumName) {
    enum class E {
        A,
        B,
        C,
    };

    static_assert(refl::enum_name(E::A) == "A");
    static_assert(refl::enum_name(E::B) == "B");
    static_assert(refl::enum_name(E::C) == "C");

    enum F {
        A,
        B,
        C,
    };

    static_assert(refl::enum_name(F::A) == "A");
    static_assert(refl::enum_name(F::B) == "B");
    static_assert(refl::enum_name(F::C) == "C");
}

TEST_CASE(NormalEnum) {
    constexpr Color invalid = {};
    static_assert(!invalid);
    static_assert(invalid.value() == Color::Invalid);

    constexpr Color red = Color::Red;
    constexpr Color green = Color::Green;
    constexpr Color blue = Color::Blue;
    constexpr Color yellow = Color::Yellow;

    static_assert(red.name() == "Red" && red.kind() == Color::Red && red.value() == 0);
    static_assert(green.name() == "Green" && green.kind() == Color::Green && green.value() == 1);
    static_assert(blue.name() == "Blue" && blue.kind() == Color::Blue && blue.value() == 2);
    static_assert(yellow.name() == "Yellow" && yellow.kind() == Color::Yellow &&
                  yellow.value() == 3);

    static_assert(red != green && red != blue && red != yellow);

    constexpr Color red2 = Color(0);
    static_assert(red == red2);

    static_assert(red.is_one_of(Color::Red));
    static_assert(!red.is_one_of(Color::Blue));
}

TEST_CASE(MaskEnum) {
    struct Mask : refl::Enum<Mask, true, uint32_t> {
        enum Kind {
            A = 0,
            B,
            C,
            D,
        };

        using Enum::Enum;
    };

    constexpr Mask invalid = {};
    static_assert(!invalid);
    static_assert(invalid.value() == 0);

    constexpr Mask mask = Mask::A;
    constexpr Mask mask2 = Mask::B;
    constexpr Mask mask3 = Mask::C;
    constexpr Mask mask4 = Mask::D;

    static_assert(mask.name() == "A" && mask.value() == 1);
    static_assert(mask2.name() == "B" && mask2.value() == 2);
    static_assert(mask3.name() == "C" && mask3.value() == 4);
    static_assert(mask4.name() == "D" && mask4.value() == 8);

    static_assert(mask != mask2 && mask != mask3 && mask != mask4);

    constexpr Mask mask5 = Mask(Mask::A, Mask::B, Mask::C, Mask::D);
    static_assert(mask5.name() == "A | B | C | D" && mask5.value() == 15);

    Mask mask6 = Mask::A;
    mask6 |= Mask::B;
    ASSERT_EQ(mask6.name(), "A | B");
    ASSERT_TRUE(bool(mask6 & Mask::A));
    ASSERT_TRUE(bool(mask6 & Mask::B));

    mask6 |= Mask::C;
    ASSERT_EQ(mask6.name(), "A | B | C");
    ASSERT_TRUE(bool(mask6 & Mask::A));
    ASSERT_TRUE(bool(mask6 & Mask::B));
    ASSERT_TRUE(bool(mask6 & Mask::C));

    constexpr Mask mask7 = Mask(Mask::A, Mask::B, Mask::C);
    static_assert(mask7 & Mask::A);
    static_assert(mask7 & Mask::B);
    static_assert(mask7 & Mask::C);
}

TEST_CASE(StringEnum) {
    constexpr StringEnum a = StringEnum::A;
    constexpr StringEnum b = StringEnum::B;
    constexpr StringEnum c = StringEnum::C;
    constexpr StringEnum d = StringEnum::D;

    static_assert(a.value() == "A");
    static_assert(b.value() == "B");
    static_assert(c.value() == "C");
    static_assert(d.value() == "D");

    static_assert(a != b && a != c && a != d);
}

};  // TEST_SUITE(Enum)

}  // namespace

}  // namespace clice::testing
