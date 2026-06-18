#include <format>
#include <string>
#include <vector>

#include "test/test.h"
#include "test/tester.h"
#include "feature/feature.h"
#include "syntax/lexer.h"

#include "kota/meta/enum.h"

namespace clice::testing {

namespace {

TEST_SUITE(document_highlight, Tester) {

bool is_control_flow_token(const Token& token, llvm::StringRef content) {
    auto text = token.text(content);
    return text == "return" || text == "break" || text == "continue" || text == "throw" ||
           text == "case" || text == "default";
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
