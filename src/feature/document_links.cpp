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
    PositionMapper converter(content, encoding);
    auto& directives = directives_it->second;
    auto* lang_opts = &unit.lang_options();

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto [fid, offset] = unit.decompose_location(loc);
        if(fid != interested || offset >= content.size())
            return;
        auto range = find_directive_argument(content, offset, lang_opts);
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
