module clice;

import :semantic.comment;
import :syntax.lexer;
import :syntax.lexical_scan;
import :unittests.utils;

namespace clice::testing {
namespace {

/// The comment text documenting the declaration spelled `decl` in
/// `content`.
std::string attached(llvm::StringRef content, llvm::StringRef decl, bool trailing = false) {
    auto begin = static_cast<std::uint32_t>(content.find(decl));
    auto comments = scan_comments(content, dialect_of());
    auto run =
        attached_comments(content,
                          comments,
                          {
                              .range = {begin, static_cast<std::uint32_t>(begin + decl.size())},
                              .trailing = trailing
    });
    return comment_text(content, run);
}

ZEST_SUITE(CommentAttachment) {

ZEST_CASE(DirectlyAbove) {
    ZEXPECT(attached("/// Adds.\n/// Twice.\nint add();\n", "int add()") == "Adds.\nTwice.");
    ZEXPECT(attached("// Plain.\nint add();\n", "int add()") == "Plain.");
}

ZEST_CASE(BlankLine) {
    ZEXPECT(attached("// ==== Section ====\n\nvoid foo();\n", "void foo()") == "");
    ZEXPECT(attached("// TODO: tidy up.\n\nint counter;\n", "int counter") == "");
    ZEXPECT(attached("/// Kept.\n\nint counter;\n", "int counter") == "Kept.");
}

ZEST_CASE(SomethingBetween) {
    ZEXPECT(attached("/// First.\nint a;\nint b;\n", "int b") == "");
    ZEXPECT(attached("/// Doc.\n#define X\nint b;\n", "int b") == "");
    ZEXPECT(attached("/// Doc.\n[[nodiscard]] int f();\n", "int f()") == "Doc.");
    ZEXPECT(attached("// Public API\npublic:\n    int f();\n", "int f()") == "");
    ZEXPECT(attached("/// Doc.\n[[gnu::cold]] int f();\n", "int f()") == "Doc.");
}

ZEST_CASE(InsideList) {
    llvm::StringRef content = "/// Copies.\ntemplate <typename T, int N>\nT copy(T value);\n";
    ZEXPECT(attached(content, "typename T") == "");
    ZEXPECT(attached(content, "int N") == "");
    ZEXPECT(attached(content, "T value") == "");
    ZEXPECT(attached("/// Picks.\ntemplate <int N = (1 > 0)>\nint pick();\n", "int pick()") ==
            "Picks.");
    ZEXPECT(attached("/// Orders.\ntemplate <bool B = 1 < 2>\nvoid order();\n", "void order()") ==
            "Orders.");
    ZEXPECT(attached("/// Holds.\nstd::vector<int> values;\n", "values") == "Holds.");
}

ZEST_CASE(Trailing) {
    llvm::StringRef content = "struct S {\n    int x; ///< The x.\n    int y; // y note\n};\n";
    ZEXPECT(attached(content, "int x", true) == "The x.");
    ZEXPECT(attached(content, "int y", true) == "y note");
    ZEXPECT(attached(content, "int y") == "");

    llvm::StringRef both = "/// Leading.\nint z; // trailing\n";
    ZEXPECT(attached(both, "int z", true) == "Leading.");
    llvm::StringRef marked = "/// Leading.\nint z; ///< Marked.\n";
    ZEXPECT(attached(marked, "int z", true) == "Marked.");
}

ZEST_CASE(TrailingContinued) {
    llvm::StringRef content = "int x; ///< First\n       ///< second.\nint y;\n";
    ZEXPECT(attached(content, "int x", true) == "First\nsecond.");
}

ZEST_CASE(FollowingCode) {
    ZEXPECT(attached("int a; // about a\nint b;\n", "int b") == "");
    ZEXPECT(attached("int done(); /* trailing\nstill trailing\n*/\nint after();\n",
                     "int after()") == "");
    ZEXPECT(attached("enum E {\n    A, ///< A.\n    B,\n};\n", "B", true) == "");
}

ZEST_CASE(StylesDoNotMerge) {
    ZEXPECT(attached("// License.\n/// Doc.\nint f();\n", "int f()") == "Doc.");
    ZEXPECT(attached("// One.\n/* Two. */\nint f();\n", "int f()") == "One.\nTwo.");
}

ZEST_CASE(LicenceNotice) {
    ZEXPECT(attached("// Copyright 2026 The Authors.\n// SPDX-License-Identifier: MIT\n"
                     "export module m;\n",
                     "module m") == "");
    ZEXPECT(attached("/* SPDX-License-Identifier: MIT */\nint f();\n", "int f()") == "");
    ZEXPECT(attached("/// Keeps the copyright notice.\nint f();\n", "int f()") ==
            "Keeps the copyright notice.");
}

};  // ZEST_SUITE(CommentAttachment)

ZEST_SUITE(CommentText) {

std::string text_of(llvm::StringRef content) {
    auto comments = scan_comments(content, dialect_of());
    return comment_text(content, comments);
}

ZEST_CASE(LineMarkers) {
    ZEXPECT(text_of("/// Summary.\n///\n///     code();\n") == "Summary.\n\n    code();");
    ZEXPECT(text_of("//! Qt style.\n") == "Qt style.");
    ZEXPECT(text_of("///< Trailing.\n") == "Trailing.");
}

ZEST_CASE(UnmarkedBlockLines) {
    ZEXPECT(text_of("/*\nFrees the buffer.\nThen clears it.\n*/") ==
            "Frees the buffer.\nThen clears it.");
    // The lines below the opener line up with its text, not with `/*`.
    ZEXPECT(text_of("/* Writes output.\n\n   Not marked with __THROW.  */") ==
            "Writes output.\n\nNot marked with __THROW.");
}

ZEST_CASE(Javadoc) {
    ZEXPECT(text_of("/**\n * Brief.\n *\n *   indented\n */") == "Brief.\n\n  indented");
    ZEXPECT(text_of("/** Block comment documentation.\n *  Second line.\n */") ==
            "Block comment documentation.\n Second line.");
    ZEXPECT(text_of("/** Example:\n *\n *     int x = 42;\n */") == "Example:\n\n    int x = 42;");
}

ZEST_CASE(QtBlock) {
    ZEXPECT(text_of("/*!\n    \\brief Shows.\n\n    More.\n*/") == "\\brief Shows.\n\nMore.");
}

ZEST_CASE(Banners) {
    ZEXPECT(text_of("//===----===//\n// Helpers\n//===----===//\n") == "Helpers");
    ZEXPECT(text_of("/*************/") == "");
}

ZEST_CASE(ClosingFenceKept) {
    ZEXPECT(text_of("/// Runs it.\n///\n/// ~~~\n/// run();\n/// ~~~\n") ==
            "Runs it.\n\n~~~\nrun();\n~~~");
}

ZEST_CASE(StarredClose) {
    ZEXPECT(text_of("/** Runs it. **/") == "Runs it.");
    ZEXPECT(text_of("/** Returns int* */") == "Returns int*");
}

};  // ZEST_SUITE(CommentText)

}  // namespace
}  // namespace clice::testing
