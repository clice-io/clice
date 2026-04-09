#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "feature/feature.h"

namespace clice::feature {

namespace {}  // namespace

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

    // Scan forward from offset to find a quoted/angled filename range.
    auto find_filename_range = [&](std::uint32_t offset) -> std::optional<LocalSourceRange> {
        auto tail = content.substr(offset);
        auto quote_pos = tail.find_first_of("<\"");
        if(quote_pos == llvm::StringRef::npos) {
            return std::nullopt;
        }
        char open = tail[quote_pos];
        char close = open == '<' ? '>' : '"';
        auto close_pos = tail.find(close, quote_pos + 1);
        if(close_pos == llvm::StringRef::npos) {
            return std::nullopt;
        }
        return LocalSourceRange(offset + static_cast<std::uint32_t>(quote_pos),
                                offset + static_cast<std::uint32_t>(close_pos + 1));
    };

    auto add_link_by_location = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested || offset >= content.size()) {
            return;
        }
        auto range = find_filename_range(offset);
        if(!range) {
            return;
        }
        protocol::DocumentLink link{.range = to_range(converter, *range)};
        link.target = target.str();
        links.push_back(std::move(link));
    };

    for(const auto& include: directives.includes) {
        if(include.fid.isValid()) {
            add_link_by_location(include.location, unit.file_path(include.fid));
        }
    }

    for(const auto& has_include: directives.has_includes) {
        if(has_include.fid.isValid()) {
            add_link_by_location(has_include.location, unit.file_path(has_include.fid));
        }
    }

    for(const auto& embed: directives.embeds) {
        if(embed.file) {
            add_link_by_location(embed.loc, embed.file->getName());
        }
    }

    for(const auto& has_embed: directives.has_embeds) {
        if(has_embed.file) {
            add_link_by_location(has_embed.loc, has_embed.file->getName());
        }
    }

    return links;
}

}  // namespace clice::feature
