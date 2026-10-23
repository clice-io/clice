module;

#include "support/prelude.h"

module clice;

import :feature.document_links;
import :feature.protocol;
import :index.query;
import :index.symbol;
import :index.tu_manifest;
import :syntax.lexical_scan;
import :syntax.source_text;
import :vfs.file_table;

namespace clice::feature {

auto document_links(CompilationUnit& unit) -> std::vector<index::DocumentLink> {
    std::vector<index::DocumentLink> links;

    auto focus = unit.focus_file();
    auto directives_it = unit.directives().find(focus.file());
    if(directives_it == unit.directives().end()) {
        return links;
    }

    auto content = focus.content();
    auto& directives = directives_it->second;

    auto add_link = [&](clang::SourceLocation loc, llvm::StringRef target) {
        auto offset = unit.seen_at(loc).offset;
        if(offset >= content.size())
            return;
        auto range = find_directive_argument(focus.lexical(), content, offset);
        if(!range)
            return;
        links.push_back(index::DocumentLink{.range = *range, .target = target.str()});
    };

    for(const auto& include: directives.includes) {
        if(include.fid.isValid()) {
            add_link(include.location, unit.file_path(include.fid));
        }
    }

    for(const auto& has_include: directives.has_includes) {
        if(has_include.file) {
            add_link(has_include.location, unit.file_path(*has_include.file));
        }
    }

    for(const auto& embed: directives.embeds) {
        if(embed.file) {
            add_link(embed.loc, unit.file_path(*embed.file));
        }
    }

    for(const auto& has_embed: directives.has_embeds) {
        if(has_embed.file) {
            add_link(has_embed.loc, unit.file_path(*has_embed.file));
        }
    }

    /// import lib.extra;
    ///        ^~~~~~~~~ links to the unit declaring the module
    /// The preprocessor reports an import before the module is loaded;
    /// the AST's import declarations carry the loaded module.
    llvm::StringMap<clang::SourceLocation> definitions;
    for(auto* decl: unit.context().local_imports()) {
        auto* module = decl->getImportedModule();
        definitions.try_emplace(module->getFullModuleName(), module->DefinitionLoc);
    }
    for(const auto& import: directives.imports) {
        auto definition = definitions.lookup(import.name);
        if(definition.isInvalid() || import.name_locations.empty()) {
            continue;
        }
        auto range = focus.produced_range(
            clang::SourceRange(import.name_locations.front(), import.name_locations.back()));
        if(!range) {
            continue;
        }
        links.push_back(index::DocumentLink{
            .range = *range,
            .target = unit.file_path(unit.file_id(definition)).str(),
        });
    }

    // Directives are collected grouped by kind; the reply promises
    // document order.
    ranges::sort(links, {}, [](const index::DocumentLink& link) { return link.range.begin; });

    return links;
}

auto document_links(SourceText& source, llvm::ArrayRef<index::IncludeEdge> edges)
    -> std::vector<index::DocumentLink> {
    auto positions = source.positions();

    std::vector<index::DocumentLink> links;
    for(const auto& edge: edges) {
        auto line = edge.line == 0 ? std::nullopt : positions.line_bounds(edge.line - 1);
        if(!line) {
            continue;
        }
        auto range = find_directive_argument(source.lexical(), source.text(), line->begin);
        if(!range) {
            continue;
        }
        links.push_back({.range = *range, .target = edge.target});
    }

    ranges::sort(links, [](const index::DocumentLink& lhs, const index::DocumentLink& rhs) {
        return std::tie(lhs.range.begin, lhs.target) < std::tie(rhs.range.begin, rhs.target);
    });
    auto duplicates =
        ranges::unique(links, [](const index::DocumentLink& lhs, const index::DocumentLink& rhs) {
            return lhs.range == rhs.range && lhs.target == rhs.target;
        });
    links.erase(duplicates.begin(), duplicates.end());

    return links;
}

auto document_links(SourceText& source, const index::IndexQuery& query, Fid file)
    -> std::vector<index::DocumentLink> {
    if(!query.shard_matching(file, source.text())) {
        return {};
    }
    return document_links(source, query.project().include_edges(file));
}

auto to_protocol(llvm::ArrayRef<index::DocumentLink> links,
                 const PositionMap& map,
                 FileTable& files) -> std::vector<protocol::DocumentLink> {
    std::vector<protocol::DocumentLink> result;
    for(const auto& link: links) {
        auto range = map.range(link.range, PositionEncoding::UTF16);
        if(!range)
            continue;
        protocol::DocumentLink out{.range = *range};
        auto path = files.display_identity(link.target);
        out.target = to_uri(path);
        out.tooltip = std::move(path);
        result.push_back(std::move(out));
    }
    return result;
}

auto directive_location(FileTable& files, const index::DocumentLink& link) -> protocol::Location {
    return protocol::Location{
        .uri = to_uri(files.display_identity(link.target)),
        .range = protocol::Range{},
    };
}

}  // namespace clice::feature
