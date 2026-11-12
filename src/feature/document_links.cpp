module clice;

import :compile.inactive_regions;
import :feature.document_links;
import :feature.protocol;
import :index.query;
import :index.shard;
import :index.symbol;
import :index.project_index.manifest;
import :syntax.lexical_scan;
import :syntax.source_text;
import :vfs.file_table;

namespace clice::feature {

auto document_links(CompilationUnit& unit) -> std::vector<feature::DocumentLink> {
    std::vector<feature::DocumentLink> links;

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
        links.push_back(feature::DocumentLink{.range = *range, .target = target.str()});
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

    // Directives are collected grouped by kind; the reply promises
    // document order.
    ranges::sort(links, {}, [](const feature::DocumentLink& link) { return link.range.begin; });

    return links;
}

auto document_links(SourceText& source, llvm::ArrayRef<index::ResolvedInclude> includes)
    -> std::vector<feature::DocumentLink> {
    auto positions = source.positions();

    std::vector<feature::DocumentLink> links;
    for(const auto& include: includes) {
        auto line = include.line == 0 ? std::nullopt : positions.line_bounds(include.line - 1);
        if(!line) {
            continue;
        }
        auto range = find_directive_argument(source.lexical(), source.text(), line->begin);
        if(!range) {
            continue;
        }
        links.push_back({.range = *range, .target = include.target});
    }

    ranges::sort(links, [](const feature::DocumentLink& lhs, const feature::DocumentLink& rhs) {
        return std::tie(lhs.range.begin, lhs.target) < std::tie(rhs.range.begin, rhs.target);
    });
    auto duplicates =
        ranges::unique(links,
                       [](const feature::DocumentLink& lhs, const feature::DocumentLink& rhs) {
                           return lhs.range == rhs.range && lhs.target == rhs.target;
                       });
    links.erase(duplicates.begin(), duplicates.end());

    return links;
}

auto document_links(SourceText& source, const index::IndexQuery& query, Fid file)
    -> std::vector<feature::DocumentLink> {
    // The included files are the stored text's: they describe the buffer
    // only while it is that text.
    auto* shard = query.project().shard(file);
    if(!shard || !shard->matches_content(source.text())) {
        return {};
    }
    return document_links(source, query.project().resolved_includes(file));
}

auto merge(std::vector<feature::DocumentLink> ast,
           llvm::ArrayRef<feature::DocumentLink> index,
           llvm::ArrayRef<std::uint32_t> skipped) -> std::vector<feature::DocumentLink> {
    auto count = ast.size();
    for(const auto& link: index) {
        if(inactive_at(skipped, link.range.begin) &&
           ranges::none_of(ast, [&](const feature::DocumentLink& known) {
               return known.range == link.range && known.target == link.target;
           })) {
            ast.push_back(link);
        }
    }
    if(ast.size() != count) {
        ranges::stable_sort(ast, {}, [](const feature::DocumentLink& link) {
            return link.range.begin;
        });
    }
    return ast;
}

auto to_protocol(llvm::ArrayRef<feature::DocumentLink> links,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 FileTable& files) -> std::vector<protocol::DocumentLink> {
    std::vector<protocol::DocumentLink> result;
    for(const auto& link: links) {
        auto range = map.range(link.range, encoding);
        if(!range)
            continue;
        result.push_back({.range = *range, .target = to_uri(files.display_identity(link.target))});
    }
    return result;
}

auto directive_location(FileTable& files, const feature::DocumentLink& link) -> protocol::Location {
    return protocol::Location{
        .uri = to_uri(files.display_identity(link.target)),
        .range = protocol::Range{},
    };
}

auto link_at(llvm::ArrayRef<feature::DocumentLink> links, std::uint32_t offset)
    -> const feature::DocumentLink* {
    auto it = ranges::find_if(links, [&](const feature::DocumentLink& link) {
        return link.range.contains(offset);
    });
    return it != links.end() ? &*it : nullptr;
}

}  // namespace clice::feature
