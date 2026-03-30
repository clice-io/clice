#include "test/test.h"
#include "test/tester.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

TEST_SUITE(Directive, Tester) {

std::vector<Include> includes;
std::vector<HasInclude> has_includes;
std::vector<Condition> conditions;
std::vector<MacroRef> macros;
std::vector<Pragma> pragmas;

using u32 = std::uint32_t;

void run(llvm::StringRef code) {
    add_files("main.cpp", code);
    ASSERT_TRUE(compile("-std=c++23"));
    auto fid = unit->interested_file();
    includes = unit->directives()[fid].includes;
    has_includes = unit->directives()[fid].has_includes;
    conditions = unit->directives()[fid].conditions;
    macros = unit->directives()[fid].macros;
    pragmas = unit->directives()[fid].pragmas;
}

void EXPECT_INCLUDE(u32 index, llvm::StringRef position, llvm::StringRef path) {
    auto& include = includes[index];
    auto [_, offset] = unit->decompose_location(include.location);
    ASSERT_EQ(offset, point(position));

    /// FIXME: Implicit relative path ...
    llvm::SmallString<64> target = include.skipped ? "" : unit->file_path(include.fid);
    path::remove_dots(target);

    ASSERT_EQ(target, path);
}

void EXPECT_HAS_INL(u32 index, llvm::StringRef position, llvm::StringRef path) {
    auto& has_include = has_includes[index];
    auto [_, offset] = unit->decompose_location(has_include.location);
    ASSERT_EQ(offset, point(position));

    /// FIXME:
    llvm::SmallString<64> target =
        has_include.fid.isValid() ? unit->file_path(has_include.fid) : "";
    path::remove_dots(target);

    ASSERT_EQ(target, path);
}

void EXPECT_CON(u32 index, Condition::BranchKind kind, llvm::StringRef pos) {
    auto& condition = conditions[index];
    auto [_, offset] = unit->decompose_location(condition.loc);
    ASSERT_EQ(int(condition.kind), int(kind));
    ASSERT_EQ(offset, point(pos));
}

void EXPECT_MACRO(u32 index, MacroRef::Kind kind, llvm::StringRef position) {
    auto& macro = macros[index];
    auto [_, offset] = unit->decompose_location(macro.loc);
    ASSERT_EQ(int(macro.kind), int(kind));
    ASSERT_EQ(offset, point(position));
}

void EXPECT_PRAGMA(u32 index, Pragma::Kind kind, llvm::StringRef pos, llvm::StringRef text) {
    auto& pragma = pragmas[index];
    auto [_, offset] = unit->decompose_location(pragma.loc);
    ASSERT_EQ(int(pragma.kind), int(kind));
    ASSERT_EQ(pragma.stmt, text);
    ASSERT_EQ(offset, point(pos));
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
#$(0)include "test.h"
#$(1)include "test.h"
#$(2)include "pragma_once.h"
#$(3)include "pragma_once.h"
#$(4)include "guard_macro.h"
#$(5)include "guard_macro.h"
)cpp");

    ASSERT_EQ(includes.size(), 6U);
    EXPECT_INCLUDE(0, "0", TestVFS::path("test.h"));
    EXPECT_INCLUDE(1, "1", TestVFS::path("test.h"));
    EXPECT_INCLUDE(2, "2", TestVFS::path("pragma_once.h"));
    EXPECT_INCLUDE(3, "3", "");
    EXPECT_INCLUDE(4, "4", TestVFS::path("guard_macro.h"));
    EXPECT_INCLUDE(5, "5", "");

    /// TODO: test include source range.
};

TEST_CASE(HasInclude) {
    run(R"cpp(
#[test.h]

#[main.cpp]
#include "test.h"
#if __has_include($(0)"test.h")
#endif

#if __has_include($(1)"test2.h")
#endif
)cpp");

    ASSERT_EQ(has_includes.size(), 2U);
    EXPECT_HAS_INL(0, "0", TestVFS::path("test.h"));
    EXPECT_HAS_INL(1, "1", "");
};

TEST_CASE(Condition) {
    run(R"cpp(
#[main.cpp]
#$(0)if 0
#$(1)elif 1
#$(2)else
#$(3)endif

#$(4)ifdef name
#$(5)elifdef name
#$(6)else
#$(7)endif
)cpp");

    ASSERT_EQ(conditions.size(), 8U);
    EXPECT_CON(0, Condition::BranchKind::If, "0");
    EXPECT_CON(1, Condition::BranchKind::Elif, "1");
    EXPECT_CON(2, Condition::BranchKind::Else, "2");
    EXPECT_CON(3, Condition::BranchKind::EndIf, "3");
    EXPECT_CON(4, Condition::BranchKind::Ifdef, "4");
    EXPECT_CON(5, Condition::BranchKind::Elifdef, "5");
    EXPECT_CON(6, Condition::BranchKind::Else, "6");
    EXPECT_CON(7, Condition::BranchKind::EndIf, "7");
};

TEST_CASE(Macro) {
    run(R"cpp(
#[main.cpp]
#define $(0)expr(v) v

#ifdef $(1)expr
int x = $(2)expr(1);
#endif

#undef $(3)expr

#define $(4)expr(v) v

#ifdef $(5)expr
int y = $(6)expr($(7)expr(1));
#endif

#undef $(8)expr

)cpp");

    ASSERT_EQ(macros.size(), 9U);
    EXPECT_MACRO(0, MacroRef::Kind::Def, "0");
    EXPECT_MACRO(1, MacroRef::Kind::Ref, "1");
    EXPECT_MACRO(2, MacroRef::Kind::Ref, "2");
    EXPECT_MACRO(3, MacroRef::Kind::Undef, "3");
    EXPECT_MACRO(4, MacroRef::Kind::Def, "4");
    EXPECT_MACRO(5, MacroRef::Kind::Ref, "5");
    EXPECT_MACRO(6, MacroRef::Kind::Ref, "6");
    EXPECT_MACRO(7, MacroRef::Kind::Ref, "7");
    EXPECT_MACRO(8, MacroRef::Kind::Undef, "8");
};

TEST_CASE(Pragma) {
    run(R"cpp(
#[main.cpp]
$(0)#pragma GCC poison printf sprintf fprintf
$(1)#pragma region
$(2)#pragma endregion
)cpp");

    ASSERT_EQ(pragmas.size(), 3U);
    EXPECT_PRAGMA(0, Pragma::Kind::Other, "0", "#pragma GCC poison printf sprintf fprintf");
    EXPECT_PRAGMA(1, Pragma::Kind::Region, "1", "#pragma region");
    EXPECT_PRAGMA(2, Pragma::Kind::EndRegion, "2", "#pragma endregion");
};

};  // TEST_SUITE(Directive)

}  // namespace

}  // namespace clice::testing
