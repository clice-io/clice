#include <optional>

#include "test/test.h"
#include "test/tester.h"
#include "semantic/semantics.h"

namespace clice::testing {

namespace {

using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

ZEST_SUITE(SemanticsTable, Tester){

    std::optional<std::uint32_t> token_index_at(const Semantics& semantics, std::uint32_t offset){
        for(std::uint32_t i = 0; i < semantics.spelled_tokens().size();
            i += 1){if(semantics.token_offset(i) == offset){return i;

}  // namespace
}  // namespace clice::testing

return std::nullopt;
}

ZEST_CASE(ModuleNodes) {
    add_main("main.cpp", R"cpp(
module;
export module §(name)⟦demo⟧.core;
export int value = 1;
module :private;
)cpp");
    ASSERT(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ASSERT(modules.size() == 3U);
    ASSERT(modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ASSERT(modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(modules[2].kind == ModuleDeclaration::Kind::PrivateFragment);
    ASSERT(modules[1].name_parts.size() == 2U);

    // The declaration's written name tokens are owned by its Module node,
    // so the ownership machinery can attribute them.
    auto index = token_index_at(semantics, range("name").begin);
    ASSERT(index);
    auto owners = semantics.owners(*index);
    ASSERT(owners.size() == 1U);
    auto& node = semantics.node(owners[0]);
    ASSERT(node.node.kind() == SemanticNode::Kind::Module);
    ASSERT(node.node.get<ModuleDeclaration>() == &modules[1]);
}

ZEST_CASE(CommentNodes) {
    add_main("main.cpp", "// note\nint x = 1; /* tail */\n");
    ASSERT(compile());
    auto& semantics = unit->semantics();

    auto comments = semantics.comments();
    ASSERT(comments.size() == 2U);
    ASSERT(comments[0].kind == LexicalInfo::Comment::Kind::Line);
    ASSERT(comments[1].kind == LexicalInfo::Comment::Kind::Block);

    // Each comment is also a node; it owns no spelled tokens (the stream
    // drops comments) and carries only its payload.
    std::size_t comment_nodes = 0;
    for(auto& entry: semantics.node_entries()) {
        if(entry.node.kind() == SemanticNode::Kind::Comment) {
            ASSERT(entry.node.get<LexicalInfo::Comment>() == &comments[comment_nodes]);
            ASSERT(entry.owned == 0U);
            comment_nodes += 1;
        }
    }
    ASSERT(comment_nodes == 2U);
}

ZEST_CASE(DisabledDuplicateDeclaration) {
    // A duplicate declaration in a disabled branch fails the DefinitionLoc
    // anchor; only the live one becomes a node.
    add_main("main.cpp", R"cpp(
export §(live)⟦module⟧ app;
#if 0
export module app;
#endif
)cpp");
    ASSERT(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ASSERT(modules.size() == 1U);
    ASSERT(modules[0].kind == ModuleDeclaration::Kind::Declaration);
    ASSERT(modules[0].keyword == range("live"));
}

ZEST_CASE(NoModuleNoNodes) {
    // `module` as an ordinary identifier in a non-module unit: the lexical
    // candidates (if any) must not survive the compiler cross-check.
    add_main("main.cpp", "int module = 1;\nvoid f() { module = 2; }\n");
    ASSERT(compile());
    auto& semantics = unit->semantics();

    ASSERT(semantics.module_declarations().size() == 0U);
    for(auto& entry: semantics.node_entries()) {
        ASSERT(entry.node.kind() != SemanticNode::Kind::Module);
    }
}
}
;  // ZEST_SUITE(SemanticsTable)

}  // namespace

}  // namespace clice::testing
