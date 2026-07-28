#include <vector>

#include "test/test.h"
#include "syntax/annotation.h"

namespace clice::testing {
namespace {

// Every offset and range asserted below is computed by hand from the byte
// layout of the stripped source; annotation sigils (`§`, `⟦`, `⟧`) and any
// `§(name)` markers contribute no bytes to `content`.
TEST_SUITE(annotation) {

TEST_CASE(single_named_point) {
    auto src = AnnotatedSource::from("int §(a)x;");
    EXPECT_EQ(src.content, "int x;");
    EXPECT_EQ(src.offsets.count("a"), 1u);
    EXPECT_EQ(src.offsets.lookup("a"), 4u);
    EXPECT_TRUE(src.ranges.empty());
    EXPECT_TRUE(src.nameless_offsets.empty());
}

TEST_CASE(single_nameless_point) {
    auto src = AnnotatedSource::from("int §x;");
    EXPECT_EQ(src.content, "int x;");
    EXPECT_EQ(src.nameless_offsets, (std::vector<std::uint32_t>{4}));
    EXPECT_TRUE(src.offsets.empty());
    EXPECT_TRUE(src.ranges.empty());
}

TEST_CASE(multiple_points) {
    auto src = AnnotatedSource::from("x§y§z");
    EXPECT_EQ(src.content, "xyz");
    EXPECT_EQ(src.nameless_offsets, (std::vector<std::uint32_t>{1, 2}));
}

TEST_CASE(named_range) {
    auto src = AnnotatedSource::from("int §(r)⟦x⟧;");
    EXPECT_EQ(src.content, "int x;");
    EXPECT_EQ(src.ranges.count("r"), 1u);
    auto r = src.ranges.lookup("r");
    EXPECT_EQ(r.begin, 4u);
    EXPECT_EQ(r.end, 5u);
    EXPECT_TRUE(src.offsets.empty());
}

TEST_CASE(nameless_range) {
    auto src = AnnotatedSource::from("int §⟦x⟧;");
    EXPECT_EQ(src.content, "int x;");
    EXPECT_EQ(src.ranges.count(""), 1u);
    auto r = src.ranges.lookup("");
    EXPECT_EQ(r.begin, 4u);
    EXPECT_EQ(r.end, 5u);
}

TEST_CASE(nested_ranges) {
    auto src = AnnotatedSource::from("§(out)⟦ab§(in)⟦cd⟧ef⟧");
    EXPECT_EQ(src.content, "abcdef");
    auto out = src.ranges.lookup("out");
    EXPECT_EQ(out.begin, 0u);
    EXPECT_EQ(out.end, 6u);
    auto in = src.ranges.lookup("in");
    EXPECT_EQ(in.begin, 2u);
    EXPECT_EQ(in.end, 4u);
}

TEST_CASE(point_inside_range) {
    auto src = AnnotatedSource::from("§(r)⟦ab§(p)cd⟧");
    EXPECT_EQ(src.content, "abcd");
    EXPECT_EQ(src.offsets.lookup("p"), 2u);
    auto r = src.ranges.lookup("r");
    EXPECT_EQ(r.begin, 0u);
    EXPECT_EQ(r.end, 4u);
}

TEST_CASE(explicit_nameless_parens) {
    // `§()` is the explicit nameless point; the real `()` that follows stays
    // in the stripped source.
    auto src = AnnotatedSource::from("foo§()();");
    EXPECT_EQ(src.content, "foo();");
    EXPECT_EQ(src.nameless_offsets, (std::vector<std::uint32_t>{3}));
}

TEST_CASE(adjacent_annotations) {
    auto src = AnnotatedSource::from("§(a)§(b)⟦x⟧");
    EXPECT_EQ(src.content, "x");
    EXPECT_EQ(src.offsets.lookup("a"), 0u);
    auto b = src.ranges.lookup("b");
    EXPECT_EQ(b.begin, 0u);
    EXPECT_EQ(b.end, 1u);
}

TEST_CASE(start_and_end) {
    auto src = AnnotatedSource::from("§(s)ab§(e)");
    EXPECT_EQ(src.content, "ab");
    EXPECT_EQ(src.offsets.lookup("s"), 0u);
    EXPECT_EQ(src.offsets.lookup("e"), 2u);
}

TEST_CASE(utf8_passthrough) {
    // Box-drawing chars are 3 bytes each; the point lands at byte offset 9.
    auto src = AnnotatedSource::from("┌─┐§(m)x");
    EXPECT_EQ(src.content, "┌─┐x");
    EXPECT_EQ(src.offsets.lookup("m"), 9u);
}

TEST_CASE(doxygen_passthrough) {
    llvm::StringRef input = R"(/// @param[in] x
/// @brief ${1:placeholder} $/cancelRequest
)";
    auto src = AnnotatedSource::from(input);
    EXPECT_EQ(src.content, input);
    EXPECT_TRUE(src.offsets.empty());
    EXPECT_TRUE(src.ranges.empty());
    EXPECT_TRUE(src.nameless_offsets.empty());
}

TEST_CASE(digit_names) {
    // The migration leans on numeric names heavily (§(0), §(1)⟦...⟧).
    auto src = AnnotatedSource::from("f(§(0)42);\n§(1)⟦int⟧ x;");
    EXPECT_EQ(src.content, "f(42);\nint x;");
    EXPECT_EQ(src.offsets.lookup("0"), 2u);
    auto r = src.ranges.lookup("1");
    EXPECT_EQ(r.begin, 7u);
    EXPECT_EQ(r.end, 10u);
}

TEST_CASE(point_at_eof) {
    auto src = AnnotatedSource::from("x§");
    EXPECT_EQ(src.content, "x");
    EXPECT_EQ(src.nameless_offsets, (std::vector<std::uint32_t>{1}));
}

TEST_CASE(empty_range_body) {
    // `§⟦⟧` is a zero-width nameless range, distinct from the `§` point.
    auto src = AnnotatedSource::from("a§⟦⟧b");
    EXPECT_EQ(src.content, "ab");
    auto r = src.ranges.lookup("");
    EXPECT_EQ(r.begin, 1u);
    EXPECT_EQ(r.end, 1u);
    EXPECT_TRUE(src.nameless_offsets.empty());
}

TEST_CASE(empty_input) {
    auto src = AnnotatedSource::from("");
    EXPECT_TRUE(src.content.empty());
    EXPECT_TRUE(src.offsets.empty());
    EXPECT_TRUE(src.ranges.empty());
    EXPECT_TRUE(src.nameless_offsets.empty());
}

TEST_CASE(no_annotations) {
    llvm::StringRef input = "int main() { return 0; }";
    auto src = AnnotatedSource::from(input);
    EXPECT_EQ(src.content, input);
    EXPECT_TRUE(src.offsets.empty());
    EXPECT_TRUE(src.ranges.empty());
    EXPECT_TRUE(src.nameless_offsets.empty());
}

};  // TEST_SUITE(annotation)

// Region offsets are byte offsets into the raw input; a region spans from just
// past the begin marker line's newline to the start of the end marker line.

}  // namespace
}  // namespace clice::testing
