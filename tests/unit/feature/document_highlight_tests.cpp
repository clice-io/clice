#include <cstdint>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "syntax/lexer.h"

#include "kota/meta/enum.h"

namespace clice::testing {

namespace {

namespace protocol = feature::protocol;

TEST_SUITE(document_highlight, Tester) {

std::vector<feature::DocumentHighlight> highlights;

bool is_control_flow_token(const Token& token, llvm::StringRef content) {
    auto text = token.text(content);
    return text == "return" || text == "break" || text == "continue" || text == "throw" ||
           text == "case" || text == "default";
}

void run(llvm::StringRef code, llvm::StringRef point_name) {
    add_main("main.cpp", code);
    ASSERT_TRUE(compile_with_pch());
    highlights = feature::document_highlights(*unit, point(point_name));
}

void EXPECT_HIGHLIGHTS(std::initializer_list<llvm::StringRef> names) {
    ASSERT_EQ(highlights.size(), names.size());
    std::size_t i = 0;
    for(auto name: names) {
        ASSERT_EQ(highlights[i].range, range(name));
        ASSERT_EQ(highlights[i].kind, protocol::DocumentHighlightKind::Text);
        ++i;
    }
}

std::string highlight_ref(const feature::PositionMapper& mapper,
                          llvm::StringRef content,
                          LocalSourceRange range) {
    auto pos = mapper.to_position(range.begin);
    if(!pos)
        return {};
    auto text = content.substr(range.begin, range.length());
    return std::format("{}:{} {}", pos->line, pos->character, text);
}

std::string join_refs(const std::vector<std::string>& refs) {
    std::string result;
    for(std::size_t i = 0; i < refs.size(); ++i) {
        if(i != 0)
            result += ", ";
        result += refs[i];
    }
    return result;
}

TEST_CASE(ReturnAndThrowExitTheSameFunction) {
    run(R"cpp(
int f(int x) {
    if(x < 0)
        @ret0[$(ret)return] -x;
    if(x == 0)
        @throw0[throw] x;
    auto nested = [] {
        @nested[return] 1;
    };
    @ret1[return] x;
}
)cpp",
        "ret");

    EXPECT_HIGHLIGHTS({"ret0", "throw0", "ret1"});
}

TEST_CASE(BreakAndContinueTargetTheSameLoop) {
    run(R"cpp(
int f(int n) {
    @loop[while](n-- > 0) {
        if(n == 1)
            @break0[$(br)break];
        if(n == 2)
            @continue0[continue];
        switch(n) {
        case 3:
            @switch_break[break];
        }
        for(int i = 0; i < n; ++i) {
            @nested_break[break];
            @nested_continue[continue];
        }
    }
    return n;
}
)cpp",
        "br");

    EXPECT_HIGHLIGHTS({"loop", "break0", "continue0"});
}

TEST_CASE(SwitchBreakHighlightsTheSwitchContext) {
    run(R"cpp(
void f(int x) {
    @switch0[switch](x) {
    @case0[case] 0:
        @break0[$(br)break];
    @default0[default]:
        @break1[break];
    }
}
)cpp",
        "br");

    EXPECT_HIGHLIGHTS({"switch0", "case0", "break0"});
}

TEST_CASE(CaseHighlightsOnlyTheCurrentSwitchCaseGroup) {
    run(R"cpp(
void f(int x) {
    @switch0[switch](x) {
    @case0[case] 0:
    @case1[$(case)case] 1:
        @break0[break];
    @default0[default]:
        @throw0[throw] x;
    }
}
)cpp",
        "case");

    EXPECT_HIGHLIGHTS({"switch0", "case0", "case1", "break0"});
}

TEST_CASE(snapshot) {
    ASSERT_SNAPSHOT_GLOB(corpus_dir, "**/*.cpp", [&](std::string_view path) -> std::string {
        if(!compile_file(path))
            return "COMPILE_ERROR";

        auto content = unit->interested_content();
        feature::PositionMapper mapper(content, feature::PositionEncoding::UTF8);
        Lexer lexer(content, true, &unit->lang_options());
        std::string result;

        while(true) {
            auto token = lexer.advance();
            if(token.is_eof())
                break;
            if(!is_control_flow_token(token, content))
                continue;

            auto token_highlights = feature::document_highlights(*unit, token.range.begin);
            if(token_highlights.empty())
                continue;

            auto cursor = highlight_ref(mapper, content, token.range);
            if(cursor.empty())
                continue;

            std::vector<std::string> refs;
            for(const auto& highlight: token_highlights) {
                auto ref = highlight_ref(mapper, content, highlight.range);
                if(!ref.empty()) {
                    refs.push_back(std::format("{}", yaml_str(ref)));
                }
            }

            result += std::format("- {{ cursor: {}, highlights: [{}] }}\n",
                                  yaml_str(cursor),
                                  join_refs(refs));
        }

        if(result.empty())
            return "[]";
        result.pop_back();
        return result;
    });
}

};  // TEST_SUITE(document_highlight)

}  // namespace

}  // namespace clice::testing
