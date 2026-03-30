#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(DocumentLink, Tester) {

std::vector<protocol::DocumentLink> links;

void run(llvm::StringRef source) {
    add_files("main.cpp", source);
    ASSERT_TRUE(compile());
    links = feature::document_links(*unit, feature::PositionEncoding::UTF8);
}

auto to_local_range(const protocol::Range& range) -> LocalSourceRange {
    feature::PositionMapper converter(unit->interested_content(), feature::PositionEncoding::UTF8);
    return LocalSourceRange(*converter.to_offset(range.start), *converter.to_offset(range.end));
}

void EXPECT_LINK(std::size_t index, llvm::StringRef name, llvm::StringRef path) {
    auto& link = links[index];
    auto expected = range(name, "main.cpp");
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
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
    EXPECT_LINK(1, "1", TestVFS::path("test.h"));
    EXPECT_LINK(2, "2", TestVFS::path("pragma_once.h"));
    EXPECT_LINK(3, "3", TestVFS::path("pragma_once.h"));
    EXPECT_LINK(4, "4", TestVFS::path("guard_macro.h"));
    EXPECT_LINK(5, "5", TestVFS::path("guard_macro.h"));
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
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
    EXPECT_LINK(1, "1", TestVFS::path("test.h"));
}

};  // TEST_SUITE(DocumentLink)

}  // namespace

}  // namespace clice::testing
