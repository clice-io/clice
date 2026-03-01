#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "feature/feature.h"

namespace clice::feature {

namespace {

namespace protocol = eventide::language::protocol;

auto to_range(const PositionMapper& converter, LocalSourceRange range) -> protocol::Range {
    return protocol::Range{
        .start = converter.to_position(range.begin),
        .end = converter.to_position(range.end),
    };
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

    return links;
}

}  // namespace clice::feature
