#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"

#include "kota/meta/enum.h"

namespace clice::testing {

namespace {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

TEST_SUITE(document_symbol, Tester) {

std::vector<protocol::DocumentSymbol> symbols;

void run(llvm::StringRef code) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile_with_pch());
    symbols = feature::document_symbols(*unit, feature::PositionEncoding::UTF8);
}

auto total_size(const std::vector<protocol::DocumentSymbol>& nodes) -> std::size_t {
    std::size_t size = 0;

    std::function<void(const std::vector<protocol::DocumentSymbol>&)> visit_symbol_vector;
    std::function<void(const std::vector<std::shared_ptr<protocol::DocumentSymbol>>&)>
        visit_ptr_vector;

    visit_symbol_vector = [&](const std::vector<protocol::DocumentSymbol>& current) {
        for(const auto& node: current) {
            ++size;
            if(node.children.has_value()) {
                visit_ptr_vector(*node.children);
            }
        }
    };

    visit_ptr_vector = [&](const std::vector<std::shared_ptr<protocol::DocumentSymbol>>& current) {
        for(const auto& node: current) {
            ++size;
            if(node->children.has_value()) {
                visit_ptr_vector(*node->children);
            }
        }
    };

    visit_symbol_vector(nodes);
    return size;
}

TEST_CASE(Namespace) {
    run(R"cpp(
namespace _1 {
    namespace _2 {

    }
}

namespace _1 {
    namespace _2 {
        namespace _3 {
        }
    }
}

namespace {}

namespace _1::_2{
}
)cpp");

    ASSERT_EQ(total_size(symbols), 8U);
}

TEST_CASE(Struct) {
    run(R"cpp(
struct _1 {};
struct _2 {};

struct _3 {
    struct _4 {};
    struct _5 {};
};
)cpp");

    ASSERT_EQ(total_size(symbols), 5U);
}

TEST_CASE(Field) {
    run(R"cpp(
struct x {
    int x1;
    int x2;

    struct y {
        int y1;
        int y2;
    };

    static int x3;
};
)cpp");

    ASSERT_EQ(total_size(symbols), 7U);
}

TEST_CASE(Constructor) {
    run(R"cpp(
struct S {
    int x;

    S(): x(0) {}
    S(int x) : x(x) {}
    S(const S& s) : x(s.x) {}
    ~S() {}
};
)cpp");

    ASSERT_EQ(total_size(symbols), 6U);
}

TEST_CASE(Method) {
    run(R"cpp(
struct _0 {
    void f(int x) {}
    void f(int* x) {}
    void f1(int& x) {}
    void f2(const int& x) {}
    void f2(const _0& x) {}
    void f2(_0 x) {}
};
)cpp");

    ASSERT_EQ(total_size(symbols), 7U);
}

TEST_CASE(Enum) {
    run(R"cpp(
enum class A {
    _1,
    _2,
    _3,
};

enum B {
    _a,
    _b,
    _c,
};
)cpp");

    ASSERT_EQ(total_size(symbols), 8U);
}

TEST_CASE(TopLevelVariable) {
    run(R"cpp(
constexpr auto x = 1;
int y = 2;
)cpp");

    ASSERT_EQ(total_size(symbols), 2U);
}

TEST_CASE(Macro) {
    run(R"cpp(
#define CLASS(X) class X

CLASS(test) {
    int x = 1;
};

#define VAR(X) int X = 1;
VAR(test)
)cpp");

    ASSERT_EQ(total_size(symbols), 3U);
}

void format_document_symbols(std::string& out,
                             std::string_view content,
                             std::span<const std::uint32_t> line_starts,
                             feature::PositionEncoding encoding,
                             llvm::ArrayRef<feature::DocumentSymbol> nodes,
                             int depth) {
    lsp::LineMap map(content, line_starts, encoding);
    auto pad = std::string(depth * 2, ' ');
    for(auto& node: nodes) {
        auto kind = kota::meta::enum_name(static_cast<SymbolKind::Kind>(node.kind), "Unknown");
        auto start = map.to_position(node.range.begin);
        auto end = map.to_position(node.range.end);
        if(!start || !end)
            continue;
        auto sel_start = map.to_position(node.selection_range.begin);
        auto sel_end = map.to_position(node.selection_range.end);
        out += std::format("- {}{{ name: {}, kind: {}, range: \"{}:{}-{}:{}\"",
                           pad,
                           yaml_str(node.name),
                           kind,
                           start->line,
                           start->character,
                           end->line,
                           end->character);
        if(sel_start && sel_end) {
            out += std::format(", selection_range: \"{}:{}-{}:{}\"",
                               sel_start->line,
                               sel_start->character,
                               sel_end->line,
                               sel_end->character);
        }
        if(!node.detail.empty()) {
            out += std::format(", detail: {}", yaml_str(node.detail));
        }
        out += " }\n";
        if(!node.children.empty()) {
            format_document_symbols(out, content, line_starts, encoding, node.children, depth + 1);
        }
    }
}

TEST_CASE(snapshot) {
    ASSERT_SNAPSHOT_GLOB(test_dir + "/document_symbol",
                         "**/*.cpp",
                         [&](std::string_view path) -> std::string {
                             if(!compile_file(path))
                                 return "COMPILE_ERROR";
                             auto content = unit->interested_content();
                             auto line_starts = unit->line_starts();
                             std::string result;
                             format_document_symbols(result,
                                                     content,
                                                     line_starts,
                                                     feature::PositionEncoding::UTF8,
                                                     feature::document_symbols(*unit),
                                                     0);
                             return result;
                         });
}

};  // TEST_SUITE(document_symbol)

}  // namespace

}  // namespace clice::testing
