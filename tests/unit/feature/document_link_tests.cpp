#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

TEST_SUITE(document_link, Tester) {

std::vector<feature::DocumentLink> links;

void run(llvm::StringRef source, llvm::StringRef standard = "-std=c++17") {
    add_files("main.cpp", source);
    ASSERT_TRUE(compile(standard));
    links = feature::document_links(*unit);
}

void EXPECT_LINK(std::size_t index, llvm::StringRef name, llvm::StringRef path) {
    auto& link = links[index];
    auto expected = range(name, "main.cpp");

    ASSERT_EQ(link.range.begin, expected.begin);
    ASSERT_EQ(link.range.end, expected.end);

    llvm::SmallString<128> target(link.target.begin(), link.target.end());
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
#include §(0)⟦"test.h"§⟧
#include §(1)⟦"test.h"§⟧
#include §(2)⟦"pragma_once.h"§⟧
#include §(3)⟦"pragma_once.h"§⟧
#include §(4)⟦"guard_macro.h"§⟧
#include §(5)⟦"guard_macro.h"§⟧
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
#include §(0)⟦"test.h"⟧

#if __has_include(§(1)⟦"test.h"⟧)
#endif

#if __has_include("test2.h")
#endif
)cpp");

    ASSERT_EQ(links.size(), 2U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
    EXPECT_LINK(1, "1", TestVFS::path("test.h"));
}

TEST_CASE(MacroInclude) {
    run(R"cpp(
#[test.h]

#[main.cpp]
#define HEADER "test.h"
#include §(0)⟦HEADER§⟧
)cpp");

    ASSERT_EQ(links.size(), 1U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
}

TEST_CASE(Embed) {
    run(R"cpp(
#[bytes.bin]
0123456789

#[main.cpp]
const char e[] = {
#embed §(0)⟦"bytes.bin"§⟧
};
)cpp",
        "-std=c++23");

    ASSERT_EQ(links.size(), 1U);
    EXPECT_LINK(0, "0", TestVFS::path("bytes.bin"));
}

TEST_CASE(HasEmbed) {
    run(R"cpp(
#[data.bin]
ABCDE

#[main.cpp]
#if __has_embed(§(0)⟦"data.bin"§⟧)
#endif

#if __has_embed("non_existent.bin")
#endif
)cpp",
        "-std=c++23");

    ASSERT_EQ(links.size(), 1U);
    EXPECT_LINK(0, "0", TestVFS::path("data.bin"));
}

TEST_CASE(IncludeDefinition) {
    add_file("test.h", "#pragma once\n");
    add_main("main.cpp", R"(
#include §(arg)⟦"test.h"⟧
§(inside)
int x = 0;
)");
    ASSERT_TRUE(compile());

    // Inside the include argument: the included file's location.
    auto arg = range("arg", "main.cpp");
    auto locations = feature::include_definition(*unit, arg.begin + 1);
    ASSERT_EQ(locations.size(), 1u);
    EXPECT_NE(locations[0].uri.find("test.h"), std::string::npos);
    EXPECT_EQ(locations[0].range.start.line, 0u);

    // Outside any include argument: empty.
    EXPECT_TRUE(feature::include_definition(*unit, point("inside")).empty());
}

};  // TEST_SUITE(document_link)

}  // namespace

}  // namespace clice::testing
