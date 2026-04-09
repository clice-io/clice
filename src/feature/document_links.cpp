#include <cstdint>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "syntax/lexer.h"

namespace clice::feature {

namespace {

bool is_directive_keyword(llvm::StringRef word) {
    return word == "include" || word == "include_next" || word == "import" || word == "embed" ||
           word == "__has_include" || word == "__has_include_next" || word == "__has_embed";
}

}  // namespace

auto document_links(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentLink> {
    std::vector<protocol::DocumentLink> links;

    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return links;
    }

    auto content = unit.interested_content();
    PositionMapper converter(content, encoding);
    auto& directives = directives_it->second;

    // Find the filename argument of a preprocessor directive starting from `offset`.
    // Creates a Lexer from the line start so that # at start-of-line is detected,
    // which enables header_name mode for #include and #embed automatically.
    // For __has_include/__has_embed, manually enables header_name mode after (.
    auto find_argument_range = [&](std::uint32_t offset) -> std::optional<LocalSourceRange> {
        std::uint32_t line_start = 0;
        if(offset > 0) {
            if(auto nl = content.rfind('\n', offset - 1); nl != llvm::StringRef::npos)
                line_start = static_cast<std::uint32_t>(nl + 1);
        }

        auto line = content.substr(line_start);
        Lexer lexer(line);
        bool after_has_keyword = false;

        while(true) {
            auto tok = lexer.advance();
            if(tok.is_eof() || tok.is_eod())
                break;

            auto abs_begin = line_start + tok.range.begin;
            auto abs_end = line_start + tok.range.end;

            // Detect __has_include/__has_embed to enable header_name mode after (.
            if(tok.is_identifier()) {
                auto text = tok.text(line);
                if(text == "__has_include" || text == "__has_include_next" ||
                   text == "__has_embed") {
                    after_has_keyword = true;
                    continue;
                }
            }

            if(tok.kind == clang::tok::l_paren && after_has_keyword) {
                after_has_keyword = false;
                lexer.set_header_name_mode();
                continue;
            }

            // Only return tokens at or after the directive's starting offset.
            if(abs_begin < offset)
                continue;

            if(tok.is_header_name() || tok.kind == clang::tok::string_literal)
                return LocalSourceRange(abs_begin, abs_end);

            if(tok.is_identifier() && !is_directive_keyword(tok.text(line)))
                return LocalSourceRange(abs_begin, abs_end);
        }
        return std::nullopt;
    };

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested || offset >= content.size())
            return;
        auto range = find_argument_range(offset);
        if(!range)
            return;
        protocol::DocumentLink link{.range = to_range(converter, *range)};
        link.target = target.str();
        links.push_back(std::move(link));
    };

    for(const auto& include: directives.includes) {
        if(include.fid.isValid()) {
            add_link(include.location, unit.file_path(include.fid));
        }
    }

    for(const auto& has_include: directives.has_includes) {
        if(has_include.fid.isValid()) {
            add_link(has_include.location, unit.file_path(has_include.fid));
        }
    }

    for(const auto& embed: directives.embeds) {
        if(embed.file) {
            add_link(embed.loc, embed.file->getName());
        }
    }

    for(const auto& has_embed: directives.has_embeds) {
        if(has_embed.file) {
            add_link(has_embed.loc, has_embed.file->getName());
        }
    }

    return links;
}

}  // namespace clice::feature
