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

    ASSERT(info.comments.size() == 2U);
    ASSERT(info.comments[0].kind == Comment::Kind::Line);
    ASSERT(text(content, info.comments[0].range) == "// line comment");
    ASSERT(info.comments[1].kind == Comment::Kind::Block);
    ASSERT(text(content, info.comments[1].range).starts_with("/* block"));
    ASSERT(text(content, info.comments[1].range).ends_with("comment */"));

}  // namespace

ZEST_CASE(CommentInString) {
    llvm::StringRef content = R"(const char* s = "// not a comment";)";
    auto info = lexical_scan(content);
    ASSERT(info.comments.size() == 0U);
}

ZEST_CASE(DirectiveComments) {
    llvm::StringRef content = "#include <vector> // trailing\n/* leading */ #define X 1\n";
    auto info = lexical_scan(content);

    ASSERT(info.comments.size() == 2U);
    ASSERT(info.comments[0].kind == Comment::Kind::Line);
    ASSERT(info.comments[1].kind == Comment::Kind::Block);
}

};  // ZEST_SUITE(LexicalScanComments)

ZEST_SUITE(LexicalScanModules) {

ZEST_CASE(GlobalFragment) {
    llvm::StringRef content = "module;\n#include <vector>\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ASSERT(decl.kind == ModuleDeclaration::Kind::GlobalFragment);
    ASSERT(text(content, decl.keyword) == "module");
    ASSERT(!decl.export_keyword.valid());
    ASSERT(decl.name_parts.size() == 0U);
}

ZEST_CASE(ExportDeclaration) {
    llvm::StringRef content = "export module foo.bar;\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(text(content, decl.export_keyword) == "export");
    ASSERT(text(content, decl.keyword) == "module");
    ASSERT(decl.name_parts.size() == 2U);
    ASSERT(text(content, decl.name_parts[0]) == "foo");
    ASSERT(text(content, decl.name_parts[1]) == "bar");
    ASSERT(!decl.colon.valid());
}

ZEST_CASE(ImplementationUnit) {
    llvm::StringRef content = "module a.b.c;\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(!decl.export_keyword.valid());
    ASSERT(decl.name_parts.size() == 3U);
}

ZEST_CASE(PartitionDeclaration) {
    llvm::StringRef content = "export module app:impl.detail;\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ASSERT(decl.kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(decl.name_parts.size() == 1U);
    ASSERT(text(content, decl.name_parts[0]) == "app");
    ASSERT(text(content, decl.colon) == ":");
    ASSERT(decl.partition_parts.size() == 2U);
    ASSERT(text(content, decl.partition_parts[0]) == "impl");
    ASSERT(text(content, decl.partition_parts[1]) == "detail");
}

ZEST_CASE(PrivateFragment) {
    llvm::StringRef content = "int x = 1;\nmodule : private ;\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 1U);
    auto& decl = info.modules[0];
    ASSERT(decl.kind == ModuleDeclaration::Kind::PrivateFragment);
    ASSERT(text(content, decl.keyword) == "module");
    ASSERT(text(content, decl.colon) == ":");
    ASSERT(decl.partition_parts.size() == 1U);
    ASSERT(text(content, decl.partition_parts[0]) == "private");
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

    ASSERT(info.modules.size() == 3U);
    ASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(text(content, info.modules[1].name_parts[0]) == "app");
    ASSERT(info.modules[2].kind == ModuleDeclaration::Kind::PrivateFragment);
    ASSERT(info.comments.size() == 1U);
}

ZEST_CASE(CommentInterleaved) {
    llvm::StringRef content = "/* gmf */ module;\nexport /* here */ module foo;\n";
    auto info = lexical_scan(content);

    ASSERT(info.modules.size() == 2U);
    ASSERT(info.modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ASSERT(info.modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(text(content, info.modules[1].name_parts[0]) == "foo");
    ASSERT(info.comments.size() == 2U);
}

ZEST_CASE(IncompleteDeclaration) {
    // While typing: no trailing semicolon yet.
    ASSERT(lexical_scan("export module fo").modules.size() == 1U);
    // No name yet: nothing to record.
    ASSERT(lexical_scan("export module ").modules.size() == 0U);
}

ZEST_CASE(EmptyContent) {
    auto info = lexical_scan("");
    ASSERT(info.comments.size() == 0U);
    ASSERT(info.modules.size() == 0U);
}

ZEST_CASE(NegativeControls) {
    // `module` as an ordinary identifier.
    ASSERT(lexical_scan("int module = 1;\nmodule = 2;\n").modules.size() == 0U);
    // Mid-line and mid-file bare `module;`.
    ASSERT(lexical_scan("int x;\nmodule;\n").modules.size() == 0U);
    ASSERT(lexical_scan("int x; module;\n").modules.size() == 0U);
    // Inside comments and strings.
    ASSERT(lexical_scan("// module foo;\n").modules.size() == 0U);
    ASSERT(lexical_scan("const char* s = \"module foo;\";\n").modules.size() == 0U);
    // Imports belong to the preprocessor callbacks, not to this scan.
    ASSERT(lexical_scan("import foo;\nexport import bar;\n").modules.size() == 0U);
    // Non-module export declaration.
    ASSERT(lexical_scan("export int f();\n").modules.size() == 0U);
    // An exported private fragment is not a thing.
    ASSERT(lexical_scan("export module :private;\n").modules.size() == 0U);
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

    ASSERT(info.block_directives.size() == 4U);
    ASSERT(info.block_directives[0].kind == BlockDirective::Kind::If);
    ASSERT(text(content, info.block_directives[0].range) == "#if A // first");
    ASSERT(info.block_directives[1].kind == BlockDirective::Kind::Else);
    ASSERT(text(content, info.block_directives[1].range) == "#elifdef B");
    ASSERT(info.block_directives[2].kind == BlockDirective::Kind::Else);
    ASSERT(info.block_directives[3].kind == BlockDirective::Kind::EndIf);
}

ZEST_CASE(ContinuedLine) {
    llvm::StringRef content = "#if defined(A) && \\\n    defined(B)\nint a;\n#endif";
    auto info = lexical_scan(content);

    ASSERT(info.block_directives.size() == 2U);
    ASSERT(text(content, info.block_directives[0].range) == "#if defined(A) && \\\n    defined(B)");
    ASSERT(text(content, info.block_directives[1].range) == "#endif");
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

    ASSERT(info.block_directives.size() == 5U);
    ASSERT(info.block_directives[0].kind == BlockDirective::Kind::Region);
    ASSERT(text(content, info.block_directives[0].range) == "#pragma region endregion_pair");
    ASSERT(info.block_directives[1].kind == BlockDirective::Kind::EndRegion);
    ASSERT(info.block_directives[2].kind == BlockDirective::Kind::Region);
    ASSERT(text(content, info.block_directives[2].range) == "#pragma region after_comment");
    ASSERT(info.block_directives[3].kind == BlockDirective::Kind::Region);
    ASSERT(text(content, info.block_directives[3].range) == "#pragma region");
    ASSERT(info.block_directives[4].kind == BlockDirective::Kind::EndRegion);
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

    ASSERT(info.include_directives.size() == 4U);
    ASSERT(text(content, info.include_directives[0]) == "#include <vector> // trailing");
    ASSERT(text(content, info.include_directives[1]) == R"(#include_next "next.h")");
    ASSERT(text(content, info.include_directives[2]) == R"(#  import "imported.h")");
    ASSERT(text(content, info.include_directives[3]) == R"(#include "skipped.h")");
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

    ASSERT(info.raw_strings.size() == 2U);
    ASSERT(text(content, info.raw_strings[0]) == R"x(R"(one
two)")x");
    ASSERT(text(content, info.raw_strings[1]) == R"y(u8R"x(")" inside)x"_suffix)y");
}

};  // ZEST_SUITE(LexicalScanRawStrings)

}  // namespace
}  // namespace clice::testing
