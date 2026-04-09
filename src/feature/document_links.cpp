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

    links.reserve(directives.includes.size() + directives.has_includes.size());

    for(const auto& include: directives.includes) {
        auto [fid, range] = unit.decompose_range(include.filename_range);
        if(fid != interested || !range.valid()) {
            continue;
        }

        protocol::DocumentLink link{
            .range = to_range(converter, range),
        };
        link.target = std::string(unit.file_path(include.fid));
        links.push_back(std::move(link));
    }

    for(const auto& has_include: directives.has_includes) {
        if(has_include.fid.isInvalid()) {
            continue;
        }

        auto [fid, offset] = unit.decompose_location(has_include.location);
        if(fid != interested || offset >= content.size()) {
            continue;
        }

        auto tail = content.substr(offset);
        char open = tail.front();
        if(open != '<' && open != '"') {
            continue;
        }

        char close = open == '<' ? '>' : '"';
        auto close_index = tail.find(close, 1);
        if(close_index == llvm::StringRef::npos) {
            continue;
        }

        LocalSourceRange range(offset, offset + static_cast<std::uint32_t>(close_index + 1));
        protocol::DocumentLink link{
            .range = to_range(converter, range),
        };
        link.target = std::string(unit.file_path(has_include.fid));
        links.push_back(std::move(link));
    }

    // Helper: scan forward from offset to find a quoted/angled filename range.
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

    for(const auto& embed: directives.embeds) {
        if(!embed.file) {
            continue;
        }

        auto [fid, offset] = unit.decompose_location(embed.loc);
        if(fid != interested || offset >= content.size()) {
            continue;
        }

        auto range = find_filename_range(offset);
        if(!range) {
            continue;
        }

        protocol::DocumentLink link{
            .range = to_range(converter, *range),
        };
        link.target = embed.file->getName().str();
        links.push_back(std::move(link));
    }

    for(const auto& has_embed: directives.has_embeds) {
        if(!has_embed.file) {
            continue;
        }

        auto [fid, offset] = unit.decompose_location(has_embed.loc);
        if(fid != interested || offset >= content.size()) {
            continue;
        }

        auto range = find_filename_range(offset);
        if(!range) {
            continue;
        }

        protocol::DocumentLink link{
            .range = to_range(converter, *range),
        };
        link.target = has_embed.file->getName().str();
        links.push_back(std::move(link));
    }

    return links;
}

}  // namespace clice::feature
