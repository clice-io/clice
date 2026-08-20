#include <optional>

#include "test/test.h"
#include "test/tester.h"
#include "semantic/semantics.h"

namespace clice::testing {

namespace {

using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

TEST_SUITE(SemanticsTable, Tester) {

std::optional<std::uint32_t> token_index_at(const Semantics& semantics, std::uint32_t offset) {
    for(std::uint32_t i = 0; i < semantics.spelled_tokens().size(); i += 1) {
        if(semantics.token_offset(i) == offset) {
            return i;
        }
    }
    return std::nullopt;
}

TEST_CASE(ModuleNodes) {
    add_main("main.cpp", R"cpp(
module;
export module §(name)⟦demo⟧.core;
export int value = 1;
module :private;
)cpp");
    ASSERT_TRUE(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ASSERT_EQ(modules.size(), 3U);
    ASSERT_EQ(modules[0].kind, ModuleDeclaration::Kind::GlobalFragment);
    ASSERT_EQ(modules[1].kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(modules[2].kind, ModuleDeclaration::Kind::PrivateFragment);
    ASSERT_EQ(modules[1].name_parts.size(), 2U);

    // The declaration's written name tokens are owned by its Module node,
    // so the ownership machinery can attribute them.
    auto index = token_index_at(semantics, range("name").begin);
    ASSERT_TRUE(index.has_value());
    auto owners = semantics.owners(*index);
    ASSERT_EQ(owners.size(), 1U);
    auto& node = semantics.node(owners[0]);
    ASSERT_EQ(node.node.kind(), SemanticNode::Kind::Module);
    ASSERT_EQ(node.node.get<ModuleDeclaration>(), &modules[1]);
}

TEST_CASE(CommentNodes) {
    add_main("main.cpp", "// note\nint x = 1; /* tail */\n");
    ASSERT_TRUE(compile());
    auto& semantics = unit->semantics();

    auto comments = semantics.comments();
    ASSERT_EQ(comments.size(), 2U);
    ASSERT_EQ(comments[0].kind, LexicalInfo::Comment::Kind::Line);
    ASSERT_EQ(comments[1].kind, LexicalInfo::Comment::Kind::Block);

    // Each comment is also a node; it owns no spelled tokens (the stream
    // drops comments) and carries only its payload.
    std::size_t comment_nodes = 0;
    for(auto& entry: semantics.node_entries()) {
        if(entry.node.kind() == SemanticNode::Kind::Comment) {
            ASSERT_EQ(entry.node.get<LexicalInfo::Comment>(), &comments[comment_nodes]);
            ASSERT_EQ(entry.owned, 0U);
            comment_nodes += 1;
        }
    }
    ASSERT_EQ(comment_nodes, 2U);
}

TEST_CASE(DisabledDuplicateDeclaration) {
    // A duplicate declaration in a disabled branch fails the DefinitionLoc
    // anchor; only the live one becomes a node.
    add_main("main.cpp", R"cpp(
export §(live)⟦module⟧ app;
#if 0
export module app;
#endif
)cpp");
    ASSERT_TRUE(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ASSERT_EQ(modules.size(), 1U);
    ASSERT_EQ(modules[0].kind, ModuleDeclaration::Kind::Declaration);
    ASSERT_EQ(modules[0].keyword, range("live"));
}

TEST_CASE(NoModuleNoNodes) {
    // `module` as an ordinary identifier in a non-module unit: the lexical
    // candidates (if any) must not survive the compiler cross-check.
    add_main("main.cpp", "int module = 1;\nvoid f() { module = 2; }\n");
    ASSERT_TRUE(compile());
    auto& semantics = unit->semantics();

    ASSERT_EQ(semantics.module_declarations().size(), 0U);
    for(auto& entry: semantics.node_entries()) {
        ASSERT_TRUE(entry.node.kind() != SemanticNode::Kind::Module);
    }
}

};  // TEST_SUITE(SemanticsTable)

}  // namespace

}  // namespace clice::testing
