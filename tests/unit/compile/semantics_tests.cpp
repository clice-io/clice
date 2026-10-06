module;

#include "modules/prelude.h"

module clice;

import :compile.semantics;
import :tests.unit.test.test;
import :tests.unit.test.tester;

namespace clice::testing {

namespace {

using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

ZEST_SUITE(SemanticsTable, Tester) {

std::optional<std::uint32_t> token_index_at(const Semantics& semantics, std::uint32_t offset) {
    for(std::uint32_t i = 0; i < semantics.spelled_tokens().size(); i += 1) {
        if(semantics.token_offset(i) == offset) {
            return i;
        }
    }
    return std::nullopt;
}

/// The fastest of three semantic tree builds over `uses` nested macro
/// invocations shaped like gtest's EXPECT_EQ, each the first build of a
/// fresh unit; nullopt when the source does not compile cleanly.
std::optional<std::chrono::nanoseconds> nested_macro_build_time(std::uint32_t uses) {
    std::string source = R"cpp(
namespace testing {
struct AssertionResult {
    bool ok;
    explicit operator bool() const { return ok; }
    const char* failure_message() const { return ""; }
};
struct Message {};
struct TestPartResult { enum Type { kNonFatalFailure }; };
namespace internal {
struct AssertHelper {
    AssertHelper(TestPartResult::Type, const char*, int, const char*) {}
    void operator=(const Message&) const {}
};
struct EqHelper {
    template <class A, class B>
    static AssertionResult Compare(const char*, const char*, const A& a, const B& b) {
        return AssertionResult{a == b};
    }
};
}  // namespace internal
}  // namespace testing
#define GTEST_AMBIGUOUS_ELSE_BLOCKER_ switch (0) case 0: default:
#define GTEST_MESSAGE_AT_(file, line, message, result_type) \
  ::testing::internal::AssertHelper(result_type, file, line, message) = ::testing::Message()
#define GTEST_MESSAGE_(message, result_type) GTEST_MESSAGE_AT_(__FILE__, __LINE__, message, result_type)
#define GTEST_NONFATAL_FAILURE_(message) GTEST_MESSAGE_(message, ::testing::TestPartResult::kNonFatalFailure)
#define GTEST_ASSERT_(expression, on_failure) \
  GTEST_AMBIGUOUS_ELSE_BLOCKER_ \
  if (const ::testing::AssertionResult gtest_ar = (expression)) \
    ; \
  else \
    on_failure(gtest_ar.failure_message())
#define GTEST_PRED_FORMAT2_(pred_format, v1, v2, on_failure) GTEST_ASSERT_(pred_format(#v1, #v2, v1, v2), on_failure)
#define EXPECT_PRED_FORMAT2(pred_format, v1, v2) GTEST_PRED_FORMAT2_(pred_format, v1, v2, GTEST_NONFATAL_FAILURE_)
#define EXPECT_EQ(val1, val2) EXPECT_PRED_FORMAT2(::testing::internal::EqHelper::Compare, val1, val2)
int square(int x) { return x * x; }
void test() {
)cpp";
    for(std::uint32_t i = 0; i < uses; i += 1) {
        source += std::format("    EXPECT_EQ(square({}), {});\n", i, i * i);
    }
    source += "}\n";

    auto fastest = std::chrono::nanoseconds::max();
    for(int run = 0; run < 3; run += 1) {
        clear();
        add_main("main.cpp", source);
        if(!compile() || !unit->diagnostics().empty()) {
            return std::nullopt;
        }
        auto start = std::chrono::steady_clock::now();
        unit->semantics();
        fastest = std::min(fastest, std::chrono::steady_clock::now() - start);
    }
    return fastest;
}

ZEST_CASE(ModuleNodes) {
    add_main("main.cpp", R"cpp(
module;
export module §(name)⟦demo⟧.core;
export int value = 1;
module :private;
)cpp");
    ZASSERT(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ZASSERT(modules.size() == 3U);
    ZASSERT(modules[0].kind == ModuleDeclaration::Kind::GlobalFragment);
    ZASSERT(modules[1].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(modules[2].kind == ModuleDeclaration::Kind::PrivateFragment);
    ZASSERT(modules[1].name_parts.size() == 2U);

    // The declaration's written name tokens are owned by its Module node,
    // so the ownership machinery can attribute them.
    auto index = token_index_at(semantics, range("name").begin);
    ZASSERT(index);
    auto owners = semantics.owners(*index);
    ZASSERT(owners.size() == 1U);
    auto& node = semantics.node(owners[0]);
    ZASSERT(node.node.kind() == SemanticNode::Kind::Module);
    ZASSERT(node.node.get<ModuleDeclaration>() == &modules[1]);
}

ZEST_CASE(CommentNodes) {
    add_main("main.cpp", "// note\nint x = 1; /* tail */\n");
    ZASSERT(compile());
    auto& semantics = unit->semantics();

    auto comments = semantics.comments();
    ZASSERT(comments.size() == 2U);
    ZASSERT(comments[0].kind == LexicalInfo::Comment::Kind::Line);
    ZASSERT(comments[1].kind == LexicalInfo::Comment::Kind::Block);

    // Each comment is also a node; it owns no spelled tokens (the stream
    // drops comments) and carries only its payload.
    std::size_t comment_nodes = 0;
    for(auto& entry: semantics.node_entries()) {
        if(entry.node.kind() == SemanticNode::Kind::Comment) {
            ZASSERT(entry.node.get<LexicalInfo::Comment>() == &comments[comment_nodes]);
            ZASSERT(entry.owned == 0U);
            comment_nodes += 1;
        }
    }
    ZASSERT(comment_nodes == 2U);
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
    ZASSERT(compile());
    auto& semantics = unit->semantics();

    auto modules = semantics.module_declarations();
    ZASSERT(modules.size() == 1U);
    ZASSERT(modules[0].kind == ModuleDeclaration::Kind::Declaration);
    ZASSERT(modules[0].keyword == range("live"));
}

ZEST_CASE(NoModuleNoNodes) {
    // `module` as an ordinary identifier in a non-module unit: the lexical
    // candidates (if any) must not survive the compiler cross-check.
    add_main("main.cpp", "int module = 1;\nvoid f() { module = 2; }\n");
    ZASSERT(compile());
    auto& semantics = unit->semantics();

    ZASSERT(semantics.module_declarations().size() == 0U);
    for(auto& entry: semantics.node_entries()) {
        ZASSERT(entry.node.kind() != SemanticNode::Kind::Module);
    }
}

ZEST_CASE(NestedMacroScaling) {
    // Every node looks its range up among the expanded tokens. Binary
    // searched with isBeforeInTranslationUnit, the first build over cold
    // SourceManager caches goes quadratic (over 20x for 4x the uses); a
    // linear build takes about 4x.
    auto small = nested_macro_build_time(400);
    auto large = nested_macro_build_time(1600);
    ZASSERT((small.has_value() && large.has_value()));
    ZASSERT(*large < *small * 12);
}

};  // ZEST_SUITE(SemanticsTable)

}  // namespace

}  // namespace clice::testing
