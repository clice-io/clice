#include <algorithm>
#include <vector>

#include "test/annotation.h"
#include "test/test.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

namespace protocol = eventide::ipc::protocol;

TEST_SUITE(CodeCompletion) {

std::vector<protocol::CompletionItem> items;
llvm::IntrusiveRefCntPtr<TestVFS> vfs;
std::string main_path;

void code_complete(llvm::StringRef code) {
    vfs = llvm::makeIntrusiveRefCnt<TestVFS>();

    CompilationParams params;
    auto annotation = AnnotatedSource::from(code);

    vfs->add("main.cpp", annotation.content);
    params.vfs = vfs;
    main_path = TestVFS::path("main.cpp");
    params.arguments =
        {"clang++", "-std=c++20", "-ffreestanding", "-Xclang", "-undef", main_path.c_str()};
    params.completion = {main_path, annotation.offsets.lookup("pos")};
    params.add_remapped_file(main_path, annotation.content);

    feature::CodeCompletionOptions options = {};
    items = feature::code_complete(params, options, feature::PositionEncoding::UTF8);
}

TEST_CASE(Score) {
    code_complete(R"cpp(
int foooo(int x);
int x = fo$(pos)
)cpp");

    auto it = std::ranges::find_if(items, [](const protocol::CompletionItem& item) {
        return item.label == "foooo";
    });
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->kind.has_value());
    ASSERT_EQ(*it->kind, protocol::CompletionItemKind::Function);
}

TEST_CASE(Snippet) {
    code_complete(R"cpp(
int x = tru$(pos)
)cpp");

    ASSERT_TRUE(!items.empty());
}

TEST_CASE(Overload) {
    code_complete(R"cpp(
int foooo(int x);
int foooo(int x, int y);
int x = fooo$(pos)
)cpp");

    ASSERT_TRUE(!items.empty());
}

TEST_CASE(Unqualified) {
    code_complete(R"cpp(
namespace A {
    void fooooo();
}

void bar() {
    fo$(pos)
}
)cpp");

    // Legacy parity: keep as smoke case without strict expectation.
}

TEST_CASE(Functor) {
    code_complete(R"cpp(
struct X {
    void operator() () {}
};

void bar() {
    X foo;
    fo$(pos);
}
)cpp");

    // Legacy parity: keep as smoke case without strict expectation.
}

TEST_CASE(Lambda) {
    code_complete(R"cpp(
void bar() {
    auto foo = [](int x){ };
    fo$(pos);
}
)cpp");

    // Legacy parity: keep as smoke case without strict expectation.
}

};  // TEST_SUITE(CodeCompletion)

}  // namespace

}  // namespace clice::testing
