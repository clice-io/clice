#include "Server/Convert.h"

#include "Protocol/Protocol.h"
#include "Support/Format.h"
#include "Support/JSON.h"
#include "Support/Ranges.h"

namespace clice::proto {

json::Value to_json(clice::PositionEncodingKind kind,
                    llvm::StringRef content,
                    llvm::ArrayRef<feature::SemanticToken> tokens) {
    std::vector<std::uint32_t> groups;

    auto add_token = [&](uint32_t line,
                         uint32_t character,
                         uint32_t length,
                         clice::SymbolKind kind,
                         SymbolModifiers modifiers) {
        groups.emplace_back(line);
        groups.emplace_back(character);
        groups.emplace_back(length);
        groups.emplace_back(kind.value());
        groups.emplace_back(0);
    };

    PositionConverter converter(content, kind);
    std::uint32_t last_line = 0;
    std::uint32_t last_char = 0;

    for(auto& token: tokens) {
        auto [begin_offset, end_offset] = token.range;
        auto [begin_line, begin_char] = converter.toPosition(begin_offset);
        auto [end_line, end_char] = converter.toPosition(end_offset);

        if(begin_line == end_line) [[likely]] {
            std::uint32_t line = begin_line - last_line;
            std::uint32_t character = (line == 0 ? begin_char - last_char : begin_char);
            std::uint32_t length = end_char - begin_char;
            add_token(line, character, length, token.kind, token.modifiers);
        } else {
            /// If the token spans multiple lines, split it into multiple tokens.
            auto sub_content = content.substr(begin_offset, end_offset - begin_offset);

            /// The first line is special.
            bool isFirst = true;
            /// The offset of the last line end.
            std::uint32_t last_line_offset = 0;
            /// The length of the current line.
            std::uint32_t line_length = 0;

            for(auto c: sub_content) {
                line_length += 1;
                if(c == '\n') {
                    std::uint32_t line;
                    std::uint32_t character;

                    if(isFirst) [[unlikely]] {
                        line = begin_line - last_line;
                        character = (line == 0 ? begin_char - last_char : begin_char);
                        isFirst = false;
                    } else {
                        line = 1;
                        character = 0;
                    }

                    std::uint32_t length =
                        remeasure(sub_content.substr(last_line_offset, line_length), kind);
                    add_token(line, character, length, token.kind, token.modifiers);

                    last_line_offset += line_length;
                    line_length = 0;
                }
            }

            /// Process the last line if it's not empty.
            if(line_length > 0) {
                std::uint32_t length = remeasure(sub_content.substr(last_line_offset), kind);
                add_token(1, 0, length, token.kind, token.modifiers);
            }
        }

        last_line = end_line;
        last_char = begin_char;
    }

    auto object = json::Object{
        /// The actual tokens.
        {"data", json::serialize(groups)}
    };
    return json::Value(std::move(object));
}

json::Value to_json(clice::PositionEncodingKind kind,
                    llvm::StringRef content,
                    llvm::ArrayRef<feature::CompletionItem> items) {
    PositionConverter converter(content, kind);
    converter.to_positions(items, [](auto& item) { return item.edit.range; });

    json::Array result;

    for(auto& item: items) {
        json::Object object{
            {"label", item.label},
            {"kind", static_cast<int>(item.kind)},
            {"textEdit",
             json::Object{
                 {"newText", item.edit.text},
                 {"range", json::serialize(converter.lookup(item.edit.range))},
             }},
            {"sortText", std::format("{}", item.score)},
        };
        result.emplace_back(std::move(object));
    }

    return json::Value(std::move(result));
}

}  // namespace clice::proto
