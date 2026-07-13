#include <cstdint>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "syntax/lexer.h"

namespace clice::feature {

auto document_links(CompilationUnitRef unit) -> std::vector<DocumentLink> {
    std::vector<DocumentLink> links;

    auto interested = unit.interested_file();
    auto directives_it = unit.directives().find(interested);
    if(directives_it == unit.directives().end()) {
        return links;
    }

    auto content = unit.interested_content();
    auto& directives = directives_it->second;
    auto* lang_opts = &unit.lang_options();

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested || offset >= content.size())
            return;
        auto range = find_directive_argument(content, offset, lang_opts);
        if(!range)
            return;
        links.push_back(DocumentLink{.range = *range, .target = target.str()});
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

    auto try_directive = [&](clang::SourceLocation loc, clang::FileID target) {
        if(!locations.empty() || !target.isValid()) {
            return;
        }
        auto [fid, directive_offset] = unit.decompose_location(loc);
        if(fid != interested || directive_offset >= content.size()) {
            return;
        }
        auto range = find_directive_argument(content, directive_offset, lang_opts);
        if(!range || !range->contains(offset)) {
            return;
        }
        locations.push_back(protocol::Location{
            .uri = to_uri(unit.file_path(target)),
            .range = protocol::Range{},
        });
    };

    for(const auto& include: directives_it->second.includes) {
        try_directive(include.location, include.fid);
    }
    for(const auto& has_include: directives_it->second.has_includes) {
        try_directive(has_include.location, has_include.fid);
    }
    return locations;
}

}  // namespace clice::feature
