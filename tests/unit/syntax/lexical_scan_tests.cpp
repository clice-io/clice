#include <cstddef>

#include "test/test.h"
#include "syntax/lexical_scan.h"

namespace clice::testing {
namespace {

using Comment = LexicalInfo::Comment;
using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

llvm::StringRef text(llvm::StringRef content, LocalSourceRange range) {
    return content.substr(range.begin, range.length());
}

TEST_SUITE(LexicalScanComments) {

TEST_CASE(CommentKinds) {
    llvm::StringRef content = R"(// line comment
int x = 1; /* block
comment */ int y = 2;
)";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.comments.size(), 2U);
    ASSERT_EQ(info.comments[0].kind, Comment::Kind::Line);
    ASSERT_EQ(text(content, info.comments[0].range), "// line comment");
    ASSERT_EQ(info.comments[1].kind, Comment::Kind::Block);
    ASSERT_TRUE(text(content, info.comments[1].range).starts_with("/* block"));
    ASSERT_TRUE(text(content, info.comments[1].range).ends_with("comment */"));
}

TEST_CASE(CommentInString) {
    llvm::StringRef content = R"(const char* s = "// not a comment";)";
    auto info = lexical_scan(content);
    ASSERT_EQ(info.comments.size(), 0U);
}

TEST_CASE(DirectiveComments) {
    llvm::StringRef content = "#include <vector> // trailing\n/* leading */ #define X 1\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.comments.size(), 2U);
    ASSERT_EQ(info.comments[0].kind, Comment::Kind::Line);
    ASSERT_EQ(info.comments[1].kind, Comment::Kind::Block);
}

};  // TEST_SUITE(LexicalScanComments)

TEST_SUITE(LexicalScanModules) {

TEST_CASE(GlobalFragment) {
    llvm::StringRef content = "module;\n#include <vector>\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 1U);
    auto& decl = info.modules[0];
    ASSERT_EQ(decl.kind, ModuleDeclaration::Kind::GlobalFragment);
    ASSERT_EQ(text(content, decl.keyword), "module");
    ASSERT_FALSE(decl.export_keyword.valid());
    ASSERT_EQ(decl.name_parts.size(), 0U);
}

TEST_CASE(ExportDeclaration) {
    llvm::StringRef content = "export module foo.bar;\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 1U);
    auto& decl = info.modules[0];
    ASSERT_EQ(decl.kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(text(content, decl.export_keyword), "export");
    ASSERT_EQ(text(content, decl.keyword), "module");
    ASSERT_EQ(decl.name_parts.size(), 2U);
    ASSERT_EQ(text(content, decl.name_parts[0]), "foo");
    ASSERT_EQ(text(content, decl.name_parts[1]), "bar");
    ASSERT_FALSE(decl.colon.valid());
}

TEST_CASE(ImplementationUnit) {
    llvm::StringRef content = "module a.b.c;\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 1U);
    auto& decl = info.modules[0];
    ASSERT_EQ(decl.kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_FALSE(decl.export_keyword.valid());
    ASSERT_EQ(decl.name_parts.size(), 3U);
}

TEST_CASE(PartitionDeclaration) {
    llvm::StringRef content = "export module app:impl.detail;\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 1U);
    auto& decl = info.modules[0];
    ASSERT_EQ(decl.kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(decl.name_parts.size(), 1U);
    ASSERT_EQ(text(content, decl.name_parts[0]), "app");
    ASSERT_EQ(text(content, decl.colon), ":");
    ASSERT_EQ(decl.partition_parts.size(), 2U);
    ASSERT_EQ(text(content, decl.partition_parts[0]), "impl");
    ASSERT_EQ(text(content, decl.partition_parts[1]), "detail");
}

TEST_CASE(PrivateFragment) {
    llvm::StringRef content = "int x = 1;\nmodule : private ;\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 1U);
    auto& decl = info.modules[0];
    ASSERT_EQ(decl.kind, ModuleDeclaration::Kind::PrivateFragment);
    ASSERT_EQ(text(content, decl.keyword), "module");
    ASSERT_EQ(text(content, decl.colon), ":");
    ASSERT_EQ(decl.partition_parts.size(), 1U);
    ASSERT_EQ(text(content, decl.partition_parts[0]), "private");
}

TEST_CASE(FullInterfaceUnit) {
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

    ASSERT_EQ(info.modules.size(), 3U);
    ASSERT_EQ(info.modules[0].kind, ModuleDeclaration::Kind::GlobalFragment);
    ASSERT_EQ(info.modules[1].kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(text(content, info.modules[1].name_parts[0]), "app");
    ASSERT_EQ(info.modules[2].kind, ModuleDeclaration::Kind::PrivateFragment);
    ASSERT_EQ(info.comments.size(), 1U);
}

TEST_CASE(CommentInterleaved) {
    llvm::StringRef content = "/* gmf */ module;\nexport /* here */ module foo;\n";
    auto info = lexical_scan(content);

    ASSERT_EQ(info.modules.size(), 2U);
    ASSERT_EQ(info.modules[0].kind, ModuleDeclaration::Kind::GlobalFragment);
    ASSERT_EQ(info.modules[1].kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(text(content, info.modules[1].name_parts[0]), "foo");
    ASSERT_EQ(info.comments.size(), 2U);
}

TEST_CASE(IncompleteDeclaration) {
    // While typing: no trailing semicolon yet.
    ASSERT_EQ(lexical_scan("export module fo").modules.size(), 1U);
    // No name yet: nothing to record.
    ASSERT_EQ(lexical_scan("export module ").modules.size(), 0U);
}

TEST_CASE(EmptyContent) {
    auto info = lexical_scan("");
    ASSERT_EQ(info.comments.size(), 0U);
    ASSERT_EQ(info.modules.size(), 0U);
}

TEST_CASE(NegativeControls) {
    // `module` as an ordinary identifier.
    ASSERT_EQ(lexical_scan("int module = 1;\nmodule = 2;\n").modules.size(), 0U);
    // Mid-line and mid-file bare `module;`.
    ASSERT_EQ(lexical_scan("int x;\nmodule;\n").modules.size(), 0U);
    ASSERT_EQ(lexical_scan("int x; module;\n").modules.size(), 0U);
    // Inside comments and strings.
    ASSERT_EQ(lexical_scan("// module foo;\n").modules.size(), 0U);
    ASSERT_EQ(lexical_scan("const char* s = \"module foo;\";\n").modules.size(), 0U);
    // Imports belong to the preprocessor callbacks, not to this scan.
    ASSERT_EQ(lexical_scan("import foo;\nexport import bar;\n").modules.size(), 0U);
    // Non-module export declaration.
    ASSERT_EQ(lexical_scan("export int f();\n").modules.size(), 0U);
    // An exported private fragment is not a thing.
    ASSERT_EQ(lexical_scan("export module :private;\n").modules.size(), 0U);
}

};  // TEST_SUITE(LexicalScanModules)

}  // namespace
}  // namespace clice::testing
