#include <vector>

#include "test/test.h"
#include "syntax/annotation.h"

namespace clice::testing {
namespace {

// Every offset and range asserted below is computed by hand from the byte
// layout of the stripped source; annotation sigils (`§`, `⟦`, `⟧`) and any
// `§(name)` markers contribute no bytes to `content`.
ZEST_SUITE(annotation) {

ZEST_CASE(single_named_point) {
    auto src = AnnotatedSource::from("int §(a)x;");
    EXPECT(src.content == "int x;");
    EXPECT(src.offsets.count("a") == 1u);
    EXPECT(src.offsets.lookup("a") == 4u);
    EXPECT(src.ranges.empty());
    EXPECT(src.nameless_offsets.empty());
}

ZEST_CASE(single_nameless_point) {
    auto src = AnnotatedSource::from("int §x;");
    EXPECT(src.content == "int x;");
    EXPECT(src.nameless_offsets == (std::vector<std::uint32_t>{4}));
    EXPECT(src.offsets.empty());
    EXPECT(src.ranges.empty());
}

ZEST_CASE(multiple_points) {
    auto src = AnnotatedSource::from("x§y§z");
    EXPECT(src.content == "xyz");
    EXPECT(src.nameless_offsets == (std::vector<std::uint32_t>{1, 2}));
}

ZEST_CASE(named_range) {
    auto src = AnnotatedSource::from("int §(r)⟦x⟧;");
    EXPECT(src.content == "int x;");
    EXPECT(src.ranges.count("r") == 1u);
    auto r = src.ranges.lookup("r");
    EXPECT(r.begin == 4u);
    EXPECT(r.end == 5u);
    EXPECT(src.offsets.empty());
}

ZEST_CASE(nameless_range) {
    auto src = AnnotatedSource::from("int §⟦x⟧;");
    EXPECT(src.content == "int x;");
    EXPECT(src.ranges.count("") == 1u);
    auto r = src.ranges.lookup("");
    EXPECT(r.begin == 4u);
    EXPECT(r.end == 5u);
}

ZEST_CASE(nested_ranges) {
    auto src = AnnotatedSource::from("§(out)⟦ab§(in)⟦cd⟧ef⟧");
    EXPECT(src.content == "abcdef");
    auto out = src.ranges.lookup("out");
    EXPECT(out.begin == 0u);
    EXPECT(out.end == 6u);
    auto in = src.ranges.lookup("in");
    EXPECT(in.begin == 2u);
    EXPECT(in.end == 4u);
}

ZEST_CASE(point_inside_range) {
    auto src = AnnotatedSource::from("§(r)⟦ab§(p)cd⟧");
    EXPECT(src.content == "abcd");
    EXPECT(src.offsets.lookup("p") == 2u);
    auto r = src.ranges.lookup("r");
    EXPECT(r.begin == 0u);
    EXPECT(r.end == 4u);
}

ZEST_CASE(explicit_nameless_parens) {
    // `§()` is the explicit nameless point; the real `()` that follows stays
    // in the stripped source.
    auto src = AnnotatedSource::from("foo§()();");
    EXPECT(src.content == "foo();");
    EXPECT(src.nameless_offsets == (std::vector<std::uint32_t>{3}));
}

ZEST_CASE(adjacent_annotations) {
    auto src = AnnotatedSource::from("§(a)§(b)⟦x⟧");
    EXPECT(src.content == "x");
    EXPECT(src.offsets.lookup("a") == 0u);
    auto b = src.ranges.lookup("b");
    EXPECT(b.begin == 0u);
    EXPECT(b.end == 1u);
}

ZEST_CASE(start_and_end) {
    auto src = AnnotatedSource::from("§(s)ab§(e)");
    EXPECT(src.content == "ab");
    EXPECT(src.offsets.lookup("s") == 0u);
    EXPECT(src.offsets.lookup("e") == 2u);
}

ZEST_CASE(utf8_passthrough) {
    // Box-drawing chars are 3 bytes each; the point lands at byte offset 9.
    auto src = AnnotatedSource::from("┌─┐§(m)x");
    EXPECT(src.content == "┌─┐x");
    EXPECT(src.offsets.lookup("m") == 9u);
}

ZEST_CASE(doxygen_passthrough) {
    llvm::StringRef input = R"(/// @param[in] x
/// @brief ${1:placeholder} $/cancelRequest
)";
    auto src = AnnotatedSource::from(input);
    EXPECT(src.content == input);
    EXPECT(src.offsets.empty());
    EXPECT(src.ranges.empty());
    EXPECT(src.nameless_offsets.empty());
}

ZEST_CASE(digit_names) {
    // The migration leans on numeric names heavily (§(0), §(1)⟦...⟧).
    auto src = AnnotatedSource::from("f(§(0)42);\n§(1)⟦int⟧ x;");
    EXPECT(src.content == "f(42);\nint x;");
    EXPECT(src.offsets.lookup("0") == 2u);
    auto r = src.ranges.lookup("1");
    EXPECT(r.begin == 7u);
    EXPECT(r.end == 10u);
}

ZEST_CASE(point_at_eof) {
    auto src = AnnotatedSource::from("x§");
    EXPECT(src.content == "x");
    EXPECT(src.nameless_offsets == (std::vector<std::uint32_t>{1}));
}

ZEST_CASE(empty_range_body) {
    // `§⟦⟧` is a zero-width nameless range, distinct from the `§` point.
    auto src = AnnotatedSource::from("a§⟦⟧b");
    EXPECT(src.content == "ab");
    auto r = src.ranges.lookup("");
    EXPECT(r.begin == 1u);
    EXPECT(r.end == 1u);
    EXPECT(src.nameless_offsets.empty());
}

ZEST_CASE(empty_input) {
    auto src = AnnotatedSource::from("");
    EXPECT(src.content.empty());
    EXPECT(src.offsets.empty());
    EXPECT(src.ranges.empty());
    EXPECT(src.nameless_offsets.empty());
}

ZEST_CASE(no_annotations) {
    llvm::StringRef input = "int main() { return 0; }";
    auto src = AnnotatedSource::from(input);
    EXPECT(src.content == input);
    EXPECT(src.offsets.empty());
    EXPECT(src.ranges.empty());
    EXPECT(src.nameless_offsets.empty());
}

};  // ZEST_SUITE(annotation)

// Region offsets are byte offsets into the raw input; a region spans from just
// past the begin marker line's newline to the start of the end marker line.

}  // namespace
}  // namespace clice::testing
