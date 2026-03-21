#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(DocumentLink) {

Tester tester;
std::vector<protocol::DocumentLink> links;

void run(llvm::StringRef source) {
    tester.clear();
    tester.add_files("main.cpp", source);
    ASSERT_TRUE(tester.compile());
    links = feature::document_links(*tester.unit, feature::PositionEncoding::UTF8);
}

auto to_local_range(const protocol::Range& range) -> LocalSourceRange {
    eventide::language::PositionMapper converter(tester.unit->interested_content(),
                                                 feature::PositionEncoding::UTF8);
    return LocalSourceRange(converter.to_offset(range.start), converter.to_offset(range.end));
}

void expect_link(std::size_t index, llvm::StringRef name, llvm::StringRef path) {
    auto& link = links[index];
    auto expected = tester.range(name, "main.cpp");
    auto actual = to_local_range(link.range);

    ASSERT_EQ(actual.begin, expected.begin);
    ASSERT_EQ(actual.end, expected.end);
    ASSERT_TRUE(link.target.has_value());

    llvm::SmallString<128> target(link.target->begin(), link.target->end());
    path::remove_dots(target);
    ASSERT_EQ(target, path);
}

TEST_CASE(Include) {
    run(R"cpp(
#[test.h]

#[pragma_once.h]
#pragma once

#[guard_macro.h]
#ifndef TEST3_H
#define TEST3_H
#endif

#[main.cpp]
#include @0["test.h"$]
#include @1["test.h"$]
#include @2["pragma_once.h"$]
#include @3["pragma_once.h"$]
#include @4["guard_macro.h"$]
#include @5["guard_macro.h"$]
)cpp");

    ASSERT_EQ(links.size(), 6U);
    expect_link(0, "0", "test.h");
    expect_link(1, "1", "test.h");
    expect_link(2, "2", "pragma_once.h");
    expect_link(3, "3", "pragma_once.h");
    expect_link(4, "4", "guard_macro.h");
    expect_link(5, "5", "guard_macro.h");
}

TEST_CASE(HasInclude) {
    run(R"cpp(
#[test.h]

#[main.cpp]
#include @0["test.h"]

#if __has_include(@1["test.h"])
#endif

#if __has_include("test2.h")
#endif
)cpp");

    ASSERT_EQ(links.size(), 2U);
    expect_link(0, "0", "test.h");
    expect_link(1, "1", "test.h");
}

};  // TEST_SUITE(DocumentLink)

}  // namespace

}  // namespace clice::testing
