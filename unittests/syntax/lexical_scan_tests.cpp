module;

#include "support/prelude.h"

module clice;

import :syntax.lexer;
import :syntax.lexical_scan;
import :unittests.utils;

namespace clice::testing {
namespace {

using Comment = LexicalInfo::Comment;
using ModuleDeclaration = LexicalInfo::ModuleDeclaration;
using Directive = LexicalInfo::Directive;

llvm::StringRef text(llvm::StringRef content, LocalSourceRange range) {
    return content.substr(range.begin, range.length());
}

/// The directives of `info` that open, continue or close a block.
std::vector<Directive> blocks(const LexicalInfo& info) {
    std::vector<Directive> found;
    ranges::copy_if(info.directives, std::back_inserter(found), [](const Directive& directive) {
        return directive.block();
    });
    return found;
}

std::vector<Directive> of_kind(const LexicalInfo& info, Directive::Kind kind) {
    std::vector<Directive> found;
    ranges::copy_if(info.directives, std::back_inserter(found), [&](const Directive& directive) {
        return directive.kind == kind;
    });
    return found;
}

ZEST_SUITE(LexicalScanComments) {

ZEST_CASE(CommentKinds) {
    llvm::StringRef content = R"(// line comment
int x = 1; /* block
comment */ int y = 2;
)";
    auto comments = scan_comments(content, dialect_of());

    ZASSERT(comments.size() == 2U);
    ZASSERT(comments[0].kind == Comment::Kind::Line);
    ZASSERT(text(content, comments[0].range) == "// line comment");
    ZASSERT(comments[1].kind == Comment::Kind::Block);
    ZASSERT(text(content, comments[1].range).starts_with("/* block"));
    ZASSERT(text(content, comments[1].range).ends_with("comment */"));
}

ZEST_CASE(CommentInString) {
    llvm::StringRef content = R"(const char* s = "// not a comment";)";
    auto comments = scan_comments(content, dialect_of());
    ZASSERT(comments.size() == 0U);
}

ZEST_CASE(DirectiveComments) {
    llvm::StringRef content = "#include <vector> // trailing\n/* leading */ #define X 1\n";
    auto comments = scan_comments(content, dialect_of());

    ZASSERT(comments.size() == 2U);
    ZASSERT(comments[0].kind == Comment::Kind::Line);
    ZASSERT(comments[1].kind == Comment::Kind::Block);
}

};  // ZEST_SUITE(LexicalScanComments)

ZEST_SUITE(LexicalScanModules) {

ZEST_CASE(GlobalFragment) {
    llvm::StringRef content = "module;\n#include <vector>\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(text(content, decl.keyword) == "module");
    ZASSERT(!decl.export_keyword.valid());
    ZASSERT(decl.name_parts.size() == 0U);
}

ZEST_CASE(ExportDeclaration) {
    llvm::StringRef content = "export module foo.bar;\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(text(content, decl.export_keyword) == "export");
    ZASSERT(text(content, decl.keyword) == "module");
    ZASSERT(decl.name_parts.size() == 2U);
    ZASSERT(text(content, decl.name_parts[0]) == "foo");
    ZASSERT(text(content, decl.name_parts[1]) == "bar");
    ZASSERT(!decl.colon.valid());
}

ZEST_CASE(ImplementationUnit) {
    llvm::StringRef content = "module a.b.c;\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(!decl.export_keyword.valid());
    ZASSERT(decl.name_parts.size() == 3U);
}

ZEST_CASE(PartitionDeclaration) {
    llvm::StringRef content = "export module app:impl.detail;\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(decl.name_parts.size() == 1U);
    ZASSERT(text(content, decl.name_parts[0]) == "app");
    ZASSERT(text(content, decl.colon) == ":");
    ZASSERT(decl.partition_parts.size() == 2U);
    ZASSERT(text(content, decl.partition_parts[0]) == "impl");
    ZASSERT(text(content, decl.partition_parts[1]) == "detail");
}

ZEST_CASE(PrivateFragment) {
    llvm::StringRef content = "int x = 1;\nmodule : private ;\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::PrivateFragment);
    ZASSERT(text(content, decl.keyword) == "module");
    ZASSERT(text(content, decl.colon) == ":");
    ZASSERT(decl.partition_parts.size() == 1U);
    ZASSERT(text(content, decl.partition_parts[0]) == "private");
}

ZEST_CASE(FullInterfaceUnit) {
    llvm::StringRef content = R"(// interface unit
module;
#include <vector>
export module app;
import :part;
export int f();
module :private;
int hidden = 0;
)";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 3U);
    ZASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(text(content, info.modules[1].name_parts[0]) == "app");
    ZASSERT(info.modules[2].kind == ModuleDeclaration::Kind::PrivateFragment);
}

ZEST_CASE(CommentInterleaved) {
    llvm::StringRef content = "/* gmf */ module;\nexport /* here */ module foo;\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.modules.size() == 2U);
    ZASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(text(content, info.modules[1].name_parts[0]) == "foo");
    ZASSERT(scan_comments(content, dialect_of()).size() == 2U);
}

ZEST_CASE(IncompleteDeclaration) {
    // While typing: no trailing semicolon yet.
    ZASSERT(lexical_scan("export module fo", dialect_of()).modules.size() == 1U);
    // No name yet: nothing to record.
    ZASSERT(lexical_scan("export module ", dialect_of()).modules.size() == 0U);
}

ZEST_CASE(EmptyContent) {
    ZASSERT(scan_comments("", dialect_of()).size() == 0U);
    ZASSERT(lexical_scan("", dialect_of()).modules.size() == 0U);
}

ZEST_CASE(NegativeControls) {
    // `module` as an ordinary identifier.
    ZASSERT(lexical_scan("int module = 1;\nmodule = 2;\n", dialect_of()).modules.size() == 0U);
    // Mid-line and mid-file bare `module;`.
    ZASSERT(lexical_scan("int x;\nmodule;\n", dialect_of()).modules.size() == 0U);
    ZASSERT(lexical_scan("int x; module;\n", dialect_of()).modules.size() == 0U);
    // Inside comments and strings.
    ZASSERT(lexical_scan("// module foo;\n", dialect_of()).modules.size() == 0U);
    ZASSERT(lexical_scan("const char* s = \"module foo;\";\n", dialect_of()).modules.size() == 0U);
    // Imports belong to the preprocessor callbacks, not to this scan.
    ZASSERT(lexical_scan("import foo;\nexport import bar;\n", dialect_of()).modules.size() == 0U);
    // Non-module export declaration.
    ZASSERT(lexical_scan("export int f();\n", dialect_of()).modules.size() == 0U);
    // An exported private fragment is not a thing.
    ZASSERT(lexical_scan("export module :private;\n", dialect_of()).modules.size() == 0U);
}

};  // ZEST_SUITE(LexicalScanModules)

ZEST_SUITE(LexicalScanBlockDirectives) {

ZEST_CASE(ConditionalChain) {
    llvm::StringRef content = R"(#if A // first
int a;
#elifdef B
#else
#endif
#define X 1
)";
    auto info = lexical_scan(content, dialect_of());

    auto found = blocks(info);
    ZASSERT(found.size() == 4U);
    ZASSERT(found[0].kind == Directive::Kind::If);
    ZASSERT(text(content, found[0].range) == "#if A // first");
    ZASSERT(found[1].kind == Directive::Kind::Else);
    ZASSERT(text(content, found[1].range) == "#elifdef B");
    ZASSERT(found[2].kind == Directive::Kind::Else);
    ZASSERT(found[3].kind == Directive::Kind::EndIf);
    for(const auto& directive: views::drop(found, 1)) {
        ZASSERT(directive.opening == 0U);
        ZASSERT(directive.depth == 0U);
    }
    ZASSERT(info.directives.back().kind == Directive::Kind::Define);
}

ZEST_CASE(ContinuedLine) {
    llvm::StringRef content = "#if defined(A) && \\\n    defined(B)\nint a;\n#endif";
    auto info = lexical_scan(content, dialect_of());

    auto found = blocks(info);
    ZASSERT(found.size() == 2U);
    ZASSERT(text(content, found[0].range) == "#if defined(A) && \\\n    defined(B)");
    ZASSERT(text(content, found[1].range) == "#endif");
}

ZEST_CASE(PragmaRegions) {
    llvm::StringRef content = R"(#pragma GCC poison printf
#pragma region endregion_pair
#pragma mark see endregion notes
#pragma endregion
/* spans
a line */ #pragma region after_comment
int x; /* b */ #pragma endregion
#pragma region
#pragma endregion
)";
    auto info = lexical_scan(content, dialect_of());

    auto found = blocks(info);
    ZASSERT(found.size() == 5U);
    ZASSERT(found[0].kind == Directive::Kind::Region);
    ZASSERT(text(content, found[0].range) == "#pragma region endregion_pair");
    ZASSERT(found[1].kind == Directive::Kind::EndRegion);
    ZASSERT(found[2].kind == Directive::Kind::Region);
    ZASSERT(text(content, found[2].range) == "#pragma region after_comment");
    ZASSERT(found[3].kind == Directive::Kind::Region);
    ZASSERT(text(content, found[3].range) == "#pragma region");
    ZASSERT(found[4].kind == Directive::Kind::EndRegion);
    ZASSERT(of_kind(info, Directive::Kind::PragmaMark).size() == 1U);
}

};  // ZEST_SUITE(LexicalScanBlockDirectives)

ZEST_SUITE(LexicalScanIncludes) {

ZEST_CASE(IncludeForms) {
    llvm::StringRef content = R"(#include <vector> // trailing
#include_next "next.h"
  #  import "imported.h"
#define include
#pragma include
#if 0
#include "skipped.h"
#endif
)";
    auto info = lexical_scan(content, dialect_of());

    auto found = of_kind(info, Directive::Kind::Include);
    ZASSERT(found.size() == 4U);
    ZASSERT(text(content, found[0].range) == "#include <vector> // trailing");
    ZASSERT(text(content, found[1].range) == R"(#include_next "next.h")");
    ZASSERT(text(content, found[2].range) == R"(#  import "imported.h")");
    ZASSERT(text(content, found[3].range) == R"(#include "skipped.h")");
    ZASSERT(found[3].depth == 1U);
}

};  // ZEST_SUITE(LexicalScanIncludes)

ZEST_SUITE(LexicalScanRawStrings) {

ZEST_CASE(RawStringTokens) {
    llvm::StringRef content = R"cpp(auto a = R"(one
two)";
auto b = u8R"x(")" inside)x"_suffix;
auto c = "R(not raw)";
auto R = 1;
#define RAW R"(in a directive)"
)cpp";
    clang::LangOptions lang_opts;
    lang_opts.CPlusPlus = lang_opts.CPlusPlus11 = lang_opts.RawStringLiterals = true;
    auto info = lexical_scan(content, lang_opts);

    ZASSERT(info.raw_strings.size() == 2U);
    ZASSERT(text(content, info.raw_strings[0]) == R"x(R"(one
two)")x");
    ZASSERT(text(content, info.raw_strings[1]) == R"y(u8R"x(")" inside)x"_suffix)y");
}

};  // ZEST_SUITE(LexicalScanRawStrings)

ZEST_SUITE(LexicalScanLookups) {

ZEST_CASE(TokensKeepComments) {
    llvm::StringRef content = "int a; // note\n#define X 1\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.tokens.size() == 9U);
    ZASSERT(info.tokens[3].kind == clang::tok::comment);
    ZASSERT(info.tokens.back().is_eod());
    ZASSERT(info.comments.size() == 1U);
    auto& directive = info.directives.front();
    ZASSERT(info.tokens[directive.first].is_directive_hash());
    ZASSERT(info.tokens[directive.last].is_eod());
    auto words = info.words(directive);
    ZASSERT(words.size() == 3U);
    ZASSERT(words[1].text(content) == "X");
}

ZEST_CASE(TokenAt) {
    llvm::StringRef content = "int value = 1;";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.token_at(0) == 0U);
    ZASSERT(info.token_at(5) == 1U);
    ZASSERT(info.token_at(8) == 1U);
    ZASSERT(info.token_at(9) == 2U);
    ZASSERT(info.token_at(10) == 2U);
    ZASSERT(info.token_at(static_cast<std::uint32_t>(content.size())) == info.tokens.size());
}

ZEST_CASE(DirectiveAt) {
    llvm::StringRef content = "int a;\n  #include \\\n  <vector>\n/* x\n#include <fake>\n*/\n";
    auto info = lexical_scan(content, dialect_of());

    ZASSERT(info.directives.size() == 1U);
    auto at = [&](llvm::StringRef text) {
        return info.directive_at(content, static_cast<std::uint32_t>(content.find(text)));
    };
    ZASSERT(at("  #include") == &info.directives[0]);
    ZASSERT(at("vector") == &info.directives[0]);
    ZASSERT(at("int") == nullptr);
    ZASSERT(at("fake") == nullptr);
}

ZEST_CASE(DirectiveArgument) {
    llvm::StringRef content = R"(#include <vector>
#include "local.h"
#include HEADER
#if __has_include(<optional>) && HAS(<c.h>)
#endif
#include \
"spliced.h"
int x;
)";
    auto info = lexical_scan(content, dialect_of());
    auto argument = [&](llvm::StringRef at) {
        auto range =
            find_directive_argument(info, content, static_cast<std::uint32_t>(content.find(at)));
        return range ? text(content, *range) : llvm::StringRef("(none)");
    };
    ZASSERT(argument("#include <") == "<vector>");
    ZASSERT(argument("#include \"") == R"("local.h")");
    ZASSERT(argument("#include H") == "HEADER");
    ZASSERT(argument("#if") == "<optional>");
    ZASSERT(argument("<c.h>") == "<c.h>");
    ZASSERT(argument("#include \\") == R"("spliced.h")");
    ZASSERT(argument("int x") == "(none)");
}

ZEST_CASE(MacroDefinitions) {
    auto definition = [](llvm::StringRef content) {
        auto info = lexical_scan(content, dialect_of());
        auto words = info.words(info.directives.front());
        auto parts = macro_definition(words);
        if(!parts) {
            return std::string("(none)");
        }
        auto join = [&](llvm::ArrayRef<Token> tokens) {
            std::string joined;
            for(auto& token: tokens) {
                joined += token.text(content);
            }
            return joined;
        };
        return std::format("{}|{}|{}",
                           parts->name.text(content),
                           join(parts->parameters),
                           join(parts->body));
    };
    ZASSERT(definition("#define N 1 + 2") == "N||1+2");
    ZASSERT(definition("#define F(a, b) a + b") == "F|(a,b)|a+b");
    ZASSERT(definition("#define G (a) + 1") == "G||(a)+1");
    ZASSERT(definition("#define H(a") == "H|(a|");
    ZASSERT(definition("#define") == "(none)");
}

};  // ZEST_SUITE(LexicalScanLookups)

ZEST_SUITE(LexicalScanIncludeGuard) {

/// The guard macro's name, empty without one.
std::string guard(llvm::StringRef content) {
    auto info = lexical_scan(content, dialect_of());
    return info.include_guard ? text(content, info.include_guard->name).str() : "";
}

ZEST_CASE(Forms) {
    ZASSERT(guard("#ifndef A_H\n#define A_H\nint a;\n#endif\n") == "A_H");
    ZASSERT(guard("#if !defined(A_H)\n#define A_H\n#endif\n") == "A_H");
    ZASSERT(guard("#if !defined A_H\n#define A_H\n#endif\n") == "A_H");
    ZASSERT(guard("#ifndef A_H\n#define A_H 1\n#endif\n") == "A_H");
}

ZEST_CASE(CommentsAndPragmaOnce) {
    ZASSERT(guard("// license\n#pragma once\n#ifndef A_H\n#define A_H\n#endif // A_H\n") == "A_H");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n#endif\n#pragma once\n/* end */\n") == "A_H");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n#if X\n#else\n#endif\n#endif\n") == "A_H");
}

ZEST_CASE(NoGuard) {
    ZASSERT(guard("int x;\n#ifndef A_H\n#define A_H\n#endif\n") == "");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n#endif\nint x;\n") == "");
    ZASSERT(guard("#ifndef A_H\nint x;\n#define A_H\n#endif\n") == "");
    ZASSERT(guard("#ifndef A_H\n#define B_H\n#endif\n") == "");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n#else\n#endif\n") == "");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n#endif\n#define B 1\n") == "");
    ZASSERT(guard("#ifndef A_H\n#define A_H\n") == "");
    ZASSERT(guard("#ifdef A_H\n#define A_H\n#endif\n") == "");
}

ZEST_CASE(OpeningAlone) {
    auto opens = [](llvm::StringRef content) {
        return guard_opening(content, lexical_scan(content, dialect_of())).has_value();
    };
    ZASSERT(opens("// license\n#pragma once\n#ifndef A_H\n#define A_H\n#include \"b.h\"\n"));
    ZASSERT(opens("#if !defined(A_H)\n#define A_H\n"));
    ZASSERT(!opens("#include \"b.h\"\n#ifndef A_H\n#define A_H\n"));
    ZASSERT(!opens("#ifndef A_H\n#include \"b.h\"\n#define A_H\n"));
    ZASSERT(!opens("#ifndef A_H\n#define B_H\n"));
    ZASSERT(!opens("int x;\n#ifndef A_H\n#define A_H\n"));
}

};  // ZEST_SUITE(LexicalScanIncludeGuard)

ZEST_SUITE(BracketPairing) {

/// Each pair `content`'s brackets make, as `opener...closer` offsets.
std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs(llvm::StringRef content) {
    auto info = lexical_scan(content, dialect_of());
    BracketPairs brackets;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> found;
    for(auto& token: info.tokens) {
        if(auto open = brackets.visit(token, false)) {
            found.emplace_back(open->begin, token.range.begin);
        }
    }
    return found;
}

ZEST_CASE(Nesting) {
    using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
    ZASSERT(pairs("f(a[1], {2})") == Pairs({
                                         {3, 5 },
                                         {8, 10},
                                         {1, 11}
    }));
    // A closer of another kind closes nothing.
    ZASSERT(pairs("(]") == Pairs());
    // A directive line pairs its own brackets alone.
    ZASSERT(pairs("{\n#define X (}\n}") == Pairs({
                                               {0, 15}
    }));
}

};  // ZEST_SUITE(BracketPairing)

}  // namespace
}  // namespace clice::testing
