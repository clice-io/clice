#include <vector>

#include "test/annotation.h"
#include "test/snap_region.h"
#include "test/test.h"

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
TEST_SUITE(snap_region) {

TEST_CASE(no_markers) {
    auto regions = extract_snap_regions("int x;\nint y;\n");
    EXPECT_TRUE(regions.empty());
    // With no regions the filter admits everything.
    EXPECT_TRUE(snap_region_filter(regions, LocalSourceRange{3, 5}));
    EXPECT_TRUE(snap_region_filter(regions, LocalSourceRange{100, 200}));
}

TEST_CASE(single_pair) {
    auto regions = extract_snap_regions("/// <snap:begin>\nbody\n/// <snap:end>\n");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].begin, 17u);
    EXPECT_EQ(regions[0].end, 22u);
}

TEST_CASE(two_pairs_union) {
    auto regions = extract_snap_regions(
        "/// <snap:begin>\naaa\n/// <snap:end>\nmid\n/// <snap:begin>\nbbb\n/// <snap:end>\n");
    ASSERT_EQ(regions.size(), 2u);
    EXPECT_EQ(regions[0].begin, 17u);
    EXPECT_EQ(regions[0].end, 21u);
    EXPECT_EQ(regions[1].begin, 57u);
    EXPECT_EQ(regions[1].end, 61u);
    // The filter is a union over regions.
    EXPECT_TRUE(snap_region_filter(regions, LocalSourceRange{17, 20}));
    EXPECT_TRUE(snap_region_filter(regions, LocalSourceRange{57, 60}));
    EXPECT_FALSE(snap_region_filter(regions, LocalSourceRange{36, 39}));
}

TEST_CASE(named_pair) {
    auto regions = extract_snap_regions("/// <snap:begin foo>\nx\n/// <snap:end foo>\n");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].begin, 21u);
    EXPECT_EQ(regions[0].end, 23u);
}

TEST_CASE(indented_markers) {
    auto regions = extract_snap_regions("  /// <snap:begin>\nx\n    /// <snap:end>\n");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].begin, 19u);
    EXPECT_EQ(regions[0].end, 21u);
}

TEST_CASE(mid_line_ignored) {
    // Marker-like text that is not at the line start after trimming is not a
    // marker, so no region is claimed.
    auto regions =
        extract_snap_regions("int x; // /// <snap:begin>\n// see /// <snap:begin> docs\nint y;\n");
    EXPECT_TRUE(regions.empty());
}

TEST_CASE(eof_marker_no_newline) {
    // The end marker as the final line without a trailing newline exercises
    // the last-line offset branch.
    auto regions = extract_snap_regions("/// <snap:begin>\nxy\n/// <snap:end>");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].begin, 17u);
    EXPECT_EQ(regions[0].end, 20u);
}

TEST_CASE(empty_region_body) {
    auto regions = extract_snap_regions("/// <snap:begin>\n/// <snap:end>\n");
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_EQ(regions[0].begin, 17u);
    EXPECT_EQ(regions[0].end, 17u);
}

TEST_CASE(filter_containment) {
    std::vector<LocalSourceRange> regions = {
        {10, 20}
    };
    // Fully inside.
    EXPECT_TRUE(snap_region_filter(regions, LocalSourceRange{12, 18}));
    // Straddling either boundary.
    EXPECT_FALSE(snap_region_filter(regions, LocalSourceRange{18, 25}));
    EXPECT_FALSE(snap_region_filter(regions, LocalSourceRange{5, 15}));
    // Entirely outside.
    EXPECT_FALSE(snap_region_filter(regions, LocalSourceRange{30, 40}));
    // Empty region set admits everything.
    EXPECT_TRUE(snap_region_filter({}, LocalSourceRange{30, 40}));
}

};  // TEST_SUITE(snap_region)

}  // namespace
}  // namespace clice::testing
