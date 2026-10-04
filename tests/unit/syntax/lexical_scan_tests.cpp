#include <cstddef>

#include "test/test.h"
#include "syntax/lexical_scan.h"

namespace clice::testing {
namespace {

using Comment = LexicalInfo::Comment;
using ModuleDeclaration = LexicalInfo::ModuleDeclaration;
using BlockDirective = LexicalInfo::BlockDirective;

llvm::StringRef text(llvm::StringRef content, LocalSourceRange range) {
    return content.substr(range.begin, range.length());
}

ZEST_SUITE(LexicalScanComments) {

ZEST_CASE(CommentKinds) {
    llvm::StringRef content = R"(// line comment
int x = 1; /* block
comment */ int y = 2;
)";
    auto info = lexical_scan(content);

    ZASSERT(info.comments.size() == 2U);
    ZASSERT(info.comments[0].kind == Comment::Kind::Line);
    ZASSERT(text(content, info.comments[0].range) == "// line comment");
    ZASSERT(info.comments[1].kind == Comment::Kind::Block);
    ZASSERT(text(content, info.comments[1].range).starts_with("/* block"));
    ZASSERT(text(content, info.comments[1].range).ends_with("comment */"));
}

ZEST_CASE(CommentInString) {
    llvm::StringRef content = R"(const char* s = "// not a comment";)";
    auto info = lexical_scan(content);
    ZASSERT(info.comments.size() == 0U);
}

ZEST_CASE(DirectiveComments) {
    llvm::StringRef content = "#include <vector> // trailing\n/* leading */ #define X 1\n";
    auto info = lexical_scan(content);

    ZASSERT(info.comments.size() == 2U);
    ZASSERT(info.comments[0].kind == Comment::Kind::Line);
    ZASSERT(info.comments[1].kind == Comment::Kind::Block);
}

};  // ZEST_SUITE(LexicalScanComments)

ZEST_SUITE(LexicalScanModules) {

ZEST_CASE(GlobalFragment) {
    llvm::StringRef content = "module;\n#include <vector>\n";
    auto info = lexical_scan(content);

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(text(content, decl.keyword) == "module");
    ZASSERT(!decl.export_keyword.valid());
    ZASSERT(decl.name_parts.size() == 0U);
}

ZEST_CASE(ExportDeclaration) {
    llvm::StringRef content = "export module foo.bar;\n";
    auto info = lexical_scan(content);

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
    auto info = lexical_scan(content);

    ZASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ZASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(!decl.export_keyword.valid());
    ZASSERT(decl.name_parts.size() == 3U);
}

ZEST_CASE(PartitionDeclaration) {
    llvm::StringRef content = "export module app:impl.detail;\n";
    auto info = lexical_scan(content);

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
    auto info = lexical_scan(content);

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
    auto info = lexical_scan(content);

    ZASSERT(info.modules.size() == 3U);
    ZASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(text(content, info.modules[1].name_parts[0]) == "app");
    ZASSERT(info.modules[2].kind == ModuleDeclaration::Kind::PrivateFragment);
    ZASSERT(info.comments.size() == 1U);
}

ZEST_CASE(CommentInterleaved) {
    llvm::StringRef content = "/* gmf */ module;\nexport /* here */ module foo;\n";
    auto info = lexical_scan(content);

    ZASSERT(info.modules.size() == 2U);
    ZASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(text(content, info.modules[1].name_parts[0]) == "foo");
    ZASSERT(info.comments.size() == 2U);
}

ZEST_CASE(IncompleteDeclaration) {
    // While typing: no trailing semicolon yet.
    ZASSERT(lexical_scan("export module fo").modules.size() == 1U);
    // No name yet: nothing to record.
    ZASSERT(lexical_scan("export module ").modules.size() == 0U);
}

ZEST_CASE(EmptyContent) {
    auto info = lexical_scan("");
    ZASSERT(info.comments.size() == 0U);
    ZASSERT(info.modules.size() == 0U);
}

ZEST_CASE(NegativeControls) {
    // `module` as an ordinary identifier.
    ZASSERT(lexical_scan("int module = 1;\nmodule = 2;\n").modules.size() == 0U);
    // Mid-line and mid-file bare `module;`.
    ZASSERT(lexical_scan("int x;\nmodule;\n").modules.size() == 0U);
    ZASSERT(lexical_scan("int x; module;\n").modules.size() == 0U);
    // Inside comments and strings.
    ZASSERT(lexical_scan("// module foo;\n").modules.size() == 0U);
    ZASSERT(lexical_scan("const char* s = \"module foo;\";\n").modules.size() == 0U);
    // Imports belong to the preprocessor callbacks, not to this scan.
    ZASSERT(lexical_scan("import foo;\nexport import bar;\n").modules.size() == 0U);
    // Non-module export declaration.
    ZASSERT(lexical_scan("export int f();\n").modules.size() == 0U);
    // An exported private fragment is not a thing.
    ZASSERT(lexical_scan("export module :private;\n").modules.size() == 0U);
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
    auto info = lexical_scan(content);

    ZASSERT(info.block_directives.size() == 4U);
    ZASSERT(info.block_directives[0].kind == BlockDirective::Kind::If);
    ZASSERT(text(content, info.block_directives[0].range) == "#if A // first");
    ZASSERT(info.block_directives[1].kind == BlockDirective::Kind::Else);
    ZASSERT(text(content, info.block_directives[1].range) == "#elifdef B");
    ZASSERT(info.block_directives[2].kind == BlockDirective::Kind::Else);
    ZASSERT(info.block_directives[3].kind == BlockDirective::Kind::EndIf);
}

ZEST_CASE(ContinuedLine) {
    llvm::StringRef content = "#if defined(A) && \\\n    defined(B)\nint a;\n#endif";
    auto info = lexical_scan(content);

    ZASSERT(info.block_directives.size() == 2U);
    ZASSERT(text(content, info.block_directives[0].range) ==
            "#if defined(A) && \\\n    defined(B)");
    ZASSERT(text(content, info.block_directives[1].range) == "#endif");
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
    auto info = lexical_scan(content);

    ZASSERT(info.block_directives.size() == 5U);
    ZASSERT(info.block_directives[0].kind == BlockDirective::Kind::Region);
    ZASSERT(text(content, info.block_directives[0].range) == "#pragma region endregion_pair");
    ZASSERT(info.block_directives[1].kind == BlockDirective::Kind::EndRegion);
    ZASSERT(info.block_directives[2].kind == BlockDirective::Kind::Region);
    ZASSERT(text(content, info.block_directives[2].range) == "#pragma region after_comment");
    ZASSERT(info.block_directives[3].kind == BlockDirective::Kind::Region);
    ZASSERT(text(content, info.block_directives[3].range) == "#pragma region");
    ZASSERT(info.block_directives[4].kind == BlockDirective::Kind::EndRegion);
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
    auto info = lexical_scan(content);

    ZASSERT(info.include_directives.size() == 4U);
    ZASSERT(text(content, info.include_directives[0]) == "#include <vector> // trailing");
    ZASSERT(text(content, info.include_directives[1]) == R"(#include_next "next.h")");
    ZASSERT(text(content, info.include_directives[2]) == R"(#  import "imported.h")");
    ZASSERT(text(content, info.include_directives[3]) == R"(#include "skipped.h")");
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
    auto info = lexical_scan(content, &lang_opts);

    ZASSERT(info.raw_strings.size() == 2U);
    ZASSERT(text(content, info.raw_strings[0]) == R"x(R"(one
two)")x");
    ZASSERT(text(content, info.raw_strings[1]) == R"y(u8R"x(")" inside)x"_suffix)y");
}

};  // ZEST_SUITE(LexicalScanRawStrings)

}  // namespace
}  // namespace clice::testing
