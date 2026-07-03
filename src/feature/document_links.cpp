#include <cstdint>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "syntax/lexer.h"

namespace clice::feature {

auto document_links(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentLink> {
    std::vector<protocol::DocumentLink> links;

    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return links;
    }

    auto content = unit.interested_content();
    auto& directives = directives_it->second;
    auto* lang_opts = &unit.lang_options();
    LineMap map(content, unit.line_starts(), encoding);

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested || offset >= content.size())
            return;
        auto range = find_directive_argument(content, offset, lang_opts);
        if(!range)
            return;
        auto protocol_range = to_range(map, *range);
        if(!protocol_range)
            return;
        protocol::DocumentLink link{.range = *protocol_range};
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

auto include_definition(CompilationUnitRef unit, std::uint32_t offset)
    -> std::vector<protocol::Location> {
    std::vector<protocol::Location> locations;

    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return locations;
    }

    auto content = unit.interested_content();
    auto* lang_opts = &unit.lang_options();

    for(const auto& include: directives_it->second.includes) {
        if(!include.fid.isValid()) {
            continue;
        }
        auto [fid, directive_offset] = unit.decompose_location(include.location);
        if(fid != interested || directive_offset >= content.size()) {
            continue;
        }
        auto range = find_directive_argument(content, directive_offset, lang_opts);
        if(!range || !range->contains(offset)) {
            continue;
        }
        locations.push_back(protocol::Location{
            .uri = to_uri(unit.file_path(include.fid)),
            .range = protocol::Range{},
        });
        break;
    }
    return locations;
}

}  // namespace clice::feature
