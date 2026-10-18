module;

#include "modules/prelude.h"

module clice:unittests.utils.merge_unit;

import :compile.compilation;
import :index.indexer;
import :index.shard;
import :index.tu_index;
import :index.tu_manifest;
import :project.project;
import :unittests.utils;

namespace clice::testing {

/// Mirror of the indexer's merge over in-memory sources: project symbols,
/// per-section shard blobs, and the TU manifest with its contributions —
/// so live-variant masks and staleness gates behave as in production.
/// `focus` receives the unit's own file id.
inline void merge_unit(Project& project, CompilationUnit& unit, Fid& focus) {
    auto bytes = index::build_tu_index(unit);
    auto view = index::TUIndex::from_bytes(bytes);
    ZASSERT(view.loaded());

    auto& project_index = project.project_index;
    llvm::SmallVector<Fid> file_ids_map;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        file_ids_map.push_back(project.file_table.intern(Spelling::absolute(view.path(i))));
    }
    llvm::SmallVector<index::SymbolHash> added;
    project_index.merge_symbols(view, file_ids_map, &added);
    project_index.search_pending.insert(added.begin(), added.end());
    focus = file_ids_map[view.path_count() - 1];

    // The consumed-content hash per TU-local path: the section's own
    // record where entries exist, the TU index's hash otherwise — mirroring the
    // indexer, so FileVersions match the shard generations they pin.
    llvm::SmallVector<std::uint64_t> consumed(view.path_count(), 0);
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        auto local_id = view.section_path(section);
        auto global_id = file_ids_map[local_id];
        // A section blob is already the final shard encoding: install the
        // bytes verbatim, as the indexer's first-variant path does.
        project_index.shards[global_id] = index::Shard::from_buffer(
            llvm::MemoryBuffer::getMemBufferCopy(view.section_blob(section)));
        project_index.search_pending_files.insert(global_id);
        consumed[local_id] = project_index.shards[global_id].content_hash();
    }

    llvm::SmallVector<VersionID> fv_of;
    for(std::uint32_t i = 0; i < view.path_count(); i += 1) {
        auto hash = consumed[i] != 0 ? consumed[i] : view.content_hash(i);
        fv_of.push_back(project.file_table.intern_version(file_ids_map[i], hash));
    }

    index::TUManifest manifest;
    manifest.tu_fv = fv_of[view.path_count() - 1];
    for(std::uint32_t i = 0; i < view.node_count(); i += 1) {
        auto node = view.node(i);
        manifest.nodes.push_back({.file = fv_of[node.file].raw,
                                  .parent = node.parent,
                                  .line = node.line,
                                  .skipped = node.skipped});
    }
    llvm::BitVector held(view.path_count());
    for(std::uint32_t section = 0; section < view.section_count(); section += 1) {
        manifest.contributions.emplace_back(fv_of[view.section_path(section)],
                                            view.section_hash(section));
        held.set(view.section_path(section));
    }
    for(std::uint32_t section = 0; section < view.instantiation_count(); section += 1) {
        held.set(view.instantiation_path(section));
    }
    auto shared_locals = view.shared_locals(held);
    ZASSERT(shared_locals);
    for(auto& local: *shared_locals) {
        index::SharedLocal entry{.symbol = local.symbol};
        for(auto path_id: local.paths) {
            entry.files.push_back(fv_of[path_id]);
        }
        manifest.shared_locals.push_back(std::move(entry));
    }

    for(auto path_id:
        project_index.apply_manifest(project.file_table, focus, std::move(manifest))) {
        auto it = project_index.shards.find(path_id);
        if(it != project_index.shards.end()) {
            it->second.set_live(project_index.live_variants(path_id));
        }
    }
}

}  // namespace clice::testing
