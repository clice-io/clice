module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

ZEST_SUITE(document_link, Tester) {

std::vector<index::DocumentLink> links;

void run(llvm::StringRef source, llvm::StringRef standard = "-std=c++17") {
    add_files("main.cpp", source);
    ZASSERT(compile(standard));
    links = feature::document_links(*unit);
}

void EXPECT_LINK(std::size_t index, llvm::StringRef name, llvm::StringRef path) {
    auto& link = links[index];
    auto expected = range(name, "main.cpp");

    ZASSERT(link.range.begin == expected.begin);
    ZASSERT(link.range.end == expected.end);

    ZASSERT(llvm::StringRef(link.target) == path);
}

ZEST_CASE(DirectiveArgumentFromFilename) {
    llvm::StringRef content = R"(#if __has_include("test.h"))";
    auto offset = static_cast<std::uint32_t>(content.find("test.h"));
    auto result = feature::find_directive_argument(content, offset, nullptr);

    ZASSERT(result);
    ZASSERT(content.substr(result->begin, result->length()) == R"("test.h")");
}

ZEST_CASE(Include) {
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

    ZASSERT(links.size() == 6U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
    EXPECT_LINK(1, "1", TestVFS::path("test.h"));
    EXPECT_LINK(2, "2", TestVFS::path("pragma_once.h"));
    EXPECT_LINK(3, "3", TestVFS::path("pragma_once.h"));
    EXPECT_LINK(4, "4", TestVFS::path("guard_macro.h"));
    EXPECT_LINK(5, "5", TestVFS::path("guard_macro.h"));
}

ZEST_CASE(HasInclude) {
    run(R"cpp(
#[test.h]

#[main.cpp]
#include §(0)⟦"test.h"⟧

#if __has_include(§(1)⟦"test.h"⟧)
#endif

#if __has_include("test2.h")
#endif
)cpp");

    ZASSERT(links.size() == 2U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
    EXPECT_LINK(1, "1", TestVFS::path("test.h"));
}

ZEST_CASE(MacroInclude) {
    run(R"cpp(
#[test.h]

#[main.cpp]
#define HEADER "test.h"
#include §(0)⟦HEADER§⟧
)cpp");

    ZASSERT(links.size() == 1U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
}

ZEST_CASE(HasIncludeTwice) {
    // Two operators on one line: each restarts the argument match.
    run(R"cpp(
#[a.h]

#[b.h]

#[main.cpp]
#include "a.h"
#include "b.h"
#if __has_include(§(0)⟦"a.h"⟧) && __has_include(§(1)⟦"b.h"⟧)
#endif
)cpp");

    // Two include links, then the two operator arguments.
    ZASSERT(links.size() == 4U);
    EXPECT_LINK(2, "0", TestVFS::path("a.h"));
    EXPECT_LINK(3, "1", TestVFS::path("b.h"));
}

ZEST_CASE(ImportNamedMacro) {
    // Pre-C++20, even `import` is a legal macro name; as the filename
    // argument it must link, not read as a directive keyword.
    run(R"cpp(
#[test.h]

#[main.cpp]
#define import "test.h"
#include §(0)⟦import§⟧
)cpp");

    ZASSERT(links.size() == 1U);
    EXPECT_LINK(0, "0", TestVFS::path("test.h"));
}

ZEST_CASE(Embed) {
    run(R"cpp(
#[bytes.bin]
0123456789

#[main.cpp]
const char e[] = {
#embed §(0)⟦"bytes.bin"§⟧
};
)cpp",
        "-std=c++23");

    ZASSERT(links.size() == 1U);
    EXPECT_LINK(0, "0", TestVFS::path("bytes.bin"));
}

ZEST_CASE(HasEmbed) {
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

    ZASSERT(links.size() == 1U);
    EXPECT_LINK(0, "0", TestVFS::path("data.bin"));
}

ZEST_CASE(MissingInclude) {
    run(R"cpp(
#[main.cpp]
#include "missing.h"
)cpp");

    ZASSERT(links.empty());
}

};  // ZEST_SUITE(document_link)

}  // namespace

}  // namespace clice::testing
