module clice;

import :compile.compilation;
import :index.indexing.include_tree;
import :syntax.position;
import :support.logging;

namespace clice::index {

static std::uint32_t add_include_chain(CompilationUnit& unit,
                                       clang::FileID fid,
                                       IncludeTree& tree,
                                       llvm::StringMap<std::uint32_t>& path_table) {
    auto include_loc = unit.include_location(fid);
    if(include_loc.isInvalid()) {
        return no_node;
    }

    auto [iter, success] = tree.file_nodes.try_emplace(fid, tree.nodes.size());
    if(!success) {
        return iter->second;
    }

    auto index = iter->second;

    {
        auto presumed = unit.presumed_location(include_loc);
        tree.nodes.emplace_back();
        tree.nodes[index].line = presumed.getLine();

        auto path = unit.file_path(fid);
        auto [iter, success] = path_table.try_emplace(path, tree.paths.size());
        if(success) {
            tree.paths.emplace_back(path);
        }
        tree.nodes[index].file = iter->second;

        // The node of the file CONTAINING the directive — the parent
        // consumers pair `line` with. Recursing on the directive's own fid
        // (not the presumed include loc, which names the containing
        // file's includer and sat one level off) bottoms out at no_node
        // for directives written in the focus file.
        auto parent = add_include_chain(unit, unit.file_id(include_loc), tree, path_table);
        tree.nodes[index].parent = parent;
    }

    return index;
}

IncludeTree IncludeTree::from(CompilationUnit& unit, llvm::ArrayRef<clang::FileID> indexed_fids) {
    llvm::StringMap<std::uint32_t> path_table;
    IncludeTree tree;

    // Path and node ids are assigned in first-visit order and the
    // TU index's byte hash is an identity, so the visit order must be a
    // pure function of the parse — sort every fid set that arrives in
    // DenseMap iteration order.
    auto& directives = unit.directives();
    llvm::SmallVector<clang::FileID> directive_fids;
    directive_fids.reserve(directives.size());
    for(auto fid: views::transform(directives, [](auto& entry) -> auto& { return entry.first; })) {
        directive_fids.push_back(fid);
    }
    ranges::sort(directive_fids);
    // A header's borrowed includer context is its host's to record: the
    // synthesized files name nothing on disk, and what they include is the
    // host's tree.
    for(auto fid: directive_fids) {
        for(auto& include: directives.find(fid)->second.includes) {
            if(!include.skipped && include.fid.isValid() && !unit.from_context(include.fid)) {
                auto node = add_include_chain(unit, include.fid, tree, path_table);
                tree.file_nodes[include.fid] = node;
                tree.nodes[node].macro_braces = include.macro_braces;
            }
        }
    }

    // Skipped directives after every entry, so entered nodes keep the ids
    // the recursion above assigned. Their target was entered earlier — in
    // this parse, or under a preamble PCH in the preamble's, which the
    // callbacks above never saw — so its path may still be new here.
    llvm::SmallVector<std::pair<clang::FileID, std::uint32_t>> skipped_targets;
    for(auto fid: directive_fids) {
        for(auto& include: directives.find(fid)->second.includes) {
            // A target never entered in this parse (behind a preamble PCH)
            // has no buffer to hash, and a version without a hash is never
            // fresh: leave the directive out rather than pin the unit stale.
            if(!include.skipped || !include.fid.isValid() ||
               !unit.loaded_file_content(include.fid) || unit.from_context(fid)) {
                continue;
            }
            auto parent = add_include_chain(unit, fid, tree, path_table);
            auto [iter, success] =
                path_table.try_emplace(unit.file_path(include.fid), tree.paths.size());
            if(success) {
                tree.paths.emplace_back(iter->first());
            }
            skipped_targets.push_back({include.fid, iter->second});
            tree.nodes.push_back({
                .file = iter->second,
                .parent = parent,
                .line = unit.presumed_location(include.location).getLine(),
                .skipped = true,
            });
        }
    }

    // The files holding them were entered, so their chains are real include
    // edges; a context's synthesized files name nothing on disk.
    llvm::SmallVector<std::pair<clang::FileID, Link>> links;
    for(auto fid: directive_fids) {
        if(unit.from_context(fid)) {
            continue;
        }
        auto& written = directives.find(fid)->second;
        // Where the user sees the argument, as the links the AST gives.
        auto add = [&](clang::SourceLocation location, clang::OptionalFileEntryRef target) {
            auto seen = unit.seen_at(location);
            auto position = target && seen.file == fid
                                ? unit.positions(fid).position(seen.offset, PositionEncoding::UTF8)
                                : std::nullopt;
            if(!position) {
                return;
            }
            links.push_back({
                fid,
                {
                  .line = position->line + 1,
                  .column = position->character,
                  .target = unit.file_path(*target).str(),
                  }
            });
        };
        for(auto& has_include: written.has_includes) {
            add(has_include.location, has_include.file);
        }
        for(auto& embed: written.embeds) {
            add(embed.loc, embed.file);
        }
        for(auto& has_embed: written.has_embeds) {
            add(has_embed.loc, has_embed.file);
        }
        if(!links.empty() && links.back().first == fid) {
            tree.file_nodes[fid] = add_include_chain(unit, fid, tree, path_table);
        }
    }

    llvm::SmallVector<clang::FileID> sorted_indexed(indexed_fids.begin(), indexed_fids.end());
    ranges::sort(sorted_indexed);
    for(auto fid: sorted_indexed) {
        tree.file_nodes[fid] = add_include_chain(unit, fid, tree, path_table);
    }

    auto focus = unit.focus_file().file();
    tree.file_nodes[focus] = add_include_chain(unit, focus, tree, path_table);
    tree.paths.emplace_back(unit.file_path(focus));
    for(auto& [fid, link]: links) {
        link.file = tree.path_id(fid);
        tree.links.push_back(std::move(link));
    }

    // Hash the consumed bytes per path from the compiler's own buffers.
    // Freshness checks compare the disk against these, so they must
    // describe what the index was built from — a fid whose buffer never
    // loaded here (preamble header behind a PCH) contributes no hash and
    // its consumers stay conservative.
    tree.content_hashes.assign(tree.paths.size(), 0);
    auto hash_fid = [&](clang::FileID fid, std::uint32_t path_id) {
        if(tree.content_hashes[path_id] != 0) {
            return;
        }
        if(auto content = unit.loaded_file_content(fid)) {
            tree.content_hashes[path_id] = llvm::xxh3_64bits(*content);
        }
    };
    for(auto& [fid, node]: tree.file_nodes) {
        if(node != no_node) {
            hash_fid(fid, tree.nodes[node].file);
        }
    }
    for(auto [fid, path_id]: skipped_targets) {
        hash_fid(fid, path_id);
    }
    hash_fid(focus, tree.paths.size() - 1);
    return tree;
}

std::uint32_t IncludeTree::node_of(clang::FileID fid) const {
    auto it = file_nodes.find(fid);
    if(it == file_nodes.end()) [[unlikely]] {
        LOG_WARN(
            "The include tree has no file {} of the compile; its entries go to the "
            "focus file",
            fid.getHashValue());
        return no_node;
    }
    return it->second;
}

}  // namespace clice::index
