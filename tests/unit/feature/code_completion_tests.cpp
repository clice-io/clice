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

void code_complete(llvm::StringRef code, feature::CodeCompletionOptions options = {}) {
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

    items = feature::code_complete(params, options, feature::PositionEncoding::UTF8);
}

auto find_item(llvm::StringRef label) {
    return std::ranges::find_if(items, [&](const protocol::CompletionItem& item) {
        return item.label == label;
    });
}

TEST_CASE(Score) {
    code_complete(R"cpp(
int foooo(int x);
int x = fo$(pos)
)cpp");

    auto it = find_item("foooo");
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->kind.has_value());
    ASSERT_EQ(*it->kind, protocol::CompletionItemKind::Function);
}

TEST_CASE(Signature) {
    code_complete(R"cpp(
int foooo(int x, float y);
int x = fo$(pos)
)cpp");

    auto it = find_item("foooo");
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->label_details.has_value());
    // label_details.detail should contain the parameter list.
    ASSERT_TRUE(it->label_details->detail.has_value());
    auto& sig = *it->label_details->detail;
    ASSERT_TRUE(sig.find("int") != std::string::npos);
    ASSERT_TRUE(sig.find("float") != std::string::npos);
}

TEST_CASE(ReturnType) {
    code_complete(R"cpp(
double foooo(int x);
int x = fo$(pos)
)cpp");

    auto it = find_item("foooo");
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->label_details.has_value());
    // label_details.description should contain the return type.
    ASSERT_TRUE(it->label_details->description.has_value());
    auto& ret = *it->label_details->description;
    ASSERT_TRUE(ret.find("double") != std::string::npos);
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
    // With bundling, there should be exactly one "foooo" item.
    auto count = std::ranges::count_if(items, [](const protocol::CompletionItem& item) {
        return item.label == "foooo";
    });
    ASSERT_EQ(count, 1);

    auto it = find_item("foooo");
    ASSERT_TRUE(it != items.end());
    // Bundled overload should show count in label_details.detail.
    ASSERT_TRUE(it->label_details.has_value());
    ASSERT_TRUE(it->label_details->detail.has_value());
    auto& detail = *it->label_details->detail;
    ASSERT_TRUE(detail.find("overload") != std::string::npos);
}

TEST_CASE(FilterUnderscore) {
    code_complete(R"cpp(
int _private_thing;
int public_thing;
int x = pu$(pos)
)cpp");

    // _private_thing should be filtered when prefix doesn't start with _.
    auto it = find_item("_private_thing");
    ASSERT_TRUE(it == items.end());

    auto it2 = find_item("public_thing");
    ASSERT_TRUE(it2 != items.end());
}

TEST_CASE(FilterUnderscoreExplicit) {
    code_complete(R"cpp(
int _private_thing;
int x = _p$(pos)
)cpp");

    // When user types _, underscore-prefixed symbols should appear.
    auto it = find_item("_private_thing");
    ASSERT_TRUE(it != items.end());
}

TEST_CASE(MethodSignature) {
    code_complete(R"cpp(
struct Foo {
    int bazzzz(int a, int b);
};

void bar() {
    Foo f;
    f.ba$(pos);
}
)cpp");

    auto it = find_item("bazzzz");
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->kind.has_value());
    ASSERT_EQ(*it->kind, protocol::CompletionItemKind::Method);
    ASSERT_TRUE(it->label_details.has_value());
    ASSERT_TRUE(it->label_details->detail.has_value());
    auto& sig = *it->label_details->detail;
    ASSERT_TRUE(sig.find("int") != std::string::npos);
}

TEST_CASE(DeduplicateByLabel) {
    code_complete(R"cpp(
template <typename T>
struct Foo {
    Foo() {}
    Foo(T x) {}
    Foo(T x, T y) {}
};

template <typename T>
Foo(T) -> Foo<T>;

void bar() {
    Fo$(pos)
}
)cpp");

    // In bundle mode, "Foo" should appear exactly once (as Class kind),
    // not 3 times (Class + Constructor bundle + deduction guide bundle).
    auto count = std::ranges::count_if(items, [](const protocol::CompletionItem& item) {
        return item.label == "Foo";
    });
    ASSERT_EQ(count, 1);

    auto it = find_item("Foo");
    ASSERT_TRUE(it != items.end());
    ASSERT_TRUE(it->kind.has_value());
    ASSERT_EQ(*it->kind, protocol::CompletionItemKind::Class);
}

TEST_CASE(NoBundleOverloads) {
    feature::CodeCompletionOptions opts;
    opts.bundle_overloads = false;
    code_complete(R"cpp(
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);
int x = fooo$(pos)
)cpp",
                  opts);

    // Without bundling, each overload should be a separate item.
    auto count = std::ranges::count_if(items, [](const protocol::CompletionItem& item) {
        return item.label == "foooo";
    });
    ASSERT_TRUE(count >= 3);

    // Each should have its own signature in label_details.
    for(auto& item: items) {
        if(item.label == "foooo") {
            ASSERT_TRUE(item.label_details.has_value());
            ASSERT_TRUE(item.label_details->detail.has_value());
        }
    }
}

TEST_CASE(NoBundleNoDeduplicate) {
    feature::CodeCompletionOptions opts;
    opts.bundle_overloads = false;
    code_complete(R"cpp(
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);
int x = fooo$(pos)
)cpp",
                  opts);

    // Without bundling, deduplication should NOT apply — each overload
    // should appear as a separate item.
    auto count = std::ranges::count_if(items, [](const protocol::CompletionItem& item) {
        return item.label == "foooo";
    });
    ASSERT_TRUE(count >= 3);
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
}

TEST_CASE(Functor) {
    code_complete(R"cpp(
struct X {
    void operator() () {};
};

void bar() {
    X foo;
    fo$(pos);
}
)cpp");
}

TEST_CASE(Lambda) {
    code_complete(R"cpp(
void bar() {
    auto foo = [](int x){ };
    fo$(pos);
}
)cpp");
}

};  // TEST_SUITE(CodeCompletion)

}  // namespace

}  // namespace clice::testing
