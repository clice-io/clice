#include "Test/Test.h"
#include "Test/Tester.h"

namespace clice::testing {

namespace {

TEST_SUITE(Directive) {

Tester tester;
std::vector<Include> includes;
std::vector<HasInclude> has_includes;
std::vector<Condition> conditions;
std::vector<MacroRef> macros;
std::vector<Pragma> pragmas;

using u32 = std::uint32_t;

void run(llvm::StringRef code) {
    tester.clear();
    tester.add_files("main.cpp", code);
    ASSERT_TRUE(tester.compile("-std=c++23"));
    auto fid = tester.unit->interested_file();
    includes = tester.unit->directives()[fid].includes;
    has_includes = tester.unit->directives()[fid].has_includes;
    conditions = tester.unit->directives()[fid].conditions;
    macros = tester.unit->directives()[fid].macros;
    pragmas = tester.unit->directives()[fid].pragmas;
}

void expect_include(u32 index, llvm::StringRef position, llvm::StringRef path) {
    auto& include = includes[index];
    auto [_, offset] = tester.unit->decompose_location(include.location);
    ASSERT_EQ(offset, tester.point(position));

    /// FIXME: Implicit relative path ...
    llvm::SmallString<64> target = include.skipped ? "" : tester.unit->file_path(include.fid);
    path::remove_dots(target);

    ASSERT_EQ(target, path);
}

void expect_has_inl(u32 index, llvm::StringRef position, llvm::StringRef path) {
    auto& has_include = has_includes[index];
    auto [_, offset] = tester.unit->decompose_location(has_include.location);
    ASSERT_EQ(offset, tester.point(position));

    /// FIXME:
    llvm::SmallString<64> target =
        has_include.fid.isValid() ? tester.unit->file_path(has_include.fid) : "";
    path::remove_dots(target);

    ASSERT_EQ(target, path);
}

void expect_con(u32 index, Condition::BranchKind kind, llvm::StringRef pos) {
    auto& condition = conditions[index];
    auto [_, offset] = tester.unit->decompose_location(condition.loc);
    ASSERT_EQ(int(condition.kind), int(kind));
    ASSERT_EQ(offset, tester.point(pos));
}

void expect_macro(u32 index, MacroRef::Kind kind, llvm::StringRef position) {
    auto& macro = macros[index];
    auto [_, offset] = tester.unit->decompose_location(macro.loc);
    ASSERT_EQ(int(macro.kind), int(kind));
    ASSERT_EQ(offset, tester.point(position));
}

void expect_pragma(u32 index, Pragma::Kind kind, llvm::StringRef pos, llvm::StringRef text) {
    auto& pragma = pragmas[index];
    auto [_, offset] = tester.unit->decompose_location(pragma.loc);
    ASSERT_EQ(int(pragma.kind), int(kind));
    ASSERT_EQ(pragma.stmt, text);
    ASSERT_EQ(offset, tester.point(pos));
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
    expect_include(0, "0", "test.h");
    expect_include(1, "1", "test.h");
    expect_include(2, "2", "pragma_once.h");
    expect_include(3, "3", "");
    expect_include(4, "4", "guard_macro.h");
    expect_include(5, "5", "");

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
    expect_has_inl(0, "0", "test.h");
    expect_has_inl(1, "1", "");
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
    expect_con(0, Condition::BranchKind::If, "0");
    expect_con(1, Condition::BranchKind::Elif, "1");
    expect_con(2, Condition::BranchKind::Else, "2");
    expect_con(3, Condition::BranchKind::EndIf, "3");
    expect_con(4, Condition::BranchKind::Ifdef, "4");
    expect_con(5, Condition::BranchKind::Elifdef, "5");
    expect_con(6, Condition::BranchKind::Else, "6");
    expect_con(7, Condition::BranchKind::EndIf, "7");
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
    expect_macro(0, MacroRef::Kind::Def, "0");
    expect_macro(1, MacroRef::Kind::Ref, "1");
    expect_macro(2, MacroRef::Kind::Ref, "2");
    expect_macro(3, MacroRef::Kind::Undef, "3");
    expect_macro(4, MacroRef::Kind::Def, "4");
    expect_macro(5, MacroRef::Kind::Ref, "5");
    expect_macro(6, MacroRef::Kind::Ref, "6");
    expect_macro(7, MacroRef::Kind::Ref, "7");
    expect_macro(8, MacroRef::Kind::Undef, "8");
};

TEST_CASE(Pragma) {
    run(R"cpp(
#[main.cpp]
$(0)#pragma GCC poison printf sprintf fprintf
$(1)#pragma region
$(2)#pragma endregion
)cpp");

    ASSERT_EQ(pragmas.size(), 3U);
    expect_pragma(0, Pragma::Kind::Other, "0", "#pragma GCC poison printf sprintf fprintf");
    expect_pragma(1, Pragma::Kind::Region, "1", "#pragma region");
    expect_pragma(2, Pragma::Kind::EndRegion, "2", "#pragma endregion");
};

};  // TEST_SUITE(Directive)

}  // namespace

}  // namespace clice::testing
