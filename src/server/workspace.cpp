#include "server/workspace.h"

#include <algorithm>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "support/logging.h"
#include "syntax/scan.h"

#include "llvm/Support/MemoryBuffer.h"

namespace clice {

namespace lsp = eventide::ipc::lsp;

/// Find the tightest (innermost) occurrence containing `offset` via binary search.
const static index::Occurrence* lookup_occurrence(const std::vector<index::Occurrence>& occs,
                                                  std::uint32_t offset) {
    auto it = std::ranges::lower_bound(occs, offset, {}, [](const index::Occurrence& o) {
        return o.range.end;
    });
    const index::Occurrence* best = nullptr;
    while(it != occs.end() && it->range.contains(offset)) {
        if(!best || (it->range.end - it->range.begin) < (best->range.end - best->range.begin)) {
            best = &*it;
        }
        ++it;
    }
    return best;
}

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    OpenFileIndex::find_occurrence(std::uint32_t offset) const {
    if(!mapper)
        return std::nullopt;
    auto* occ = lookup_occurrence(file_index.occurrences, offset);
    if(!occ)
        return std::nullopt;
    auto start = mapper->to_position(occ->range.begin);
    auto end = mapper->to_position(occ->range.end);
    if(!start || !end)
        return std::nullopt;
    return std::pair{
        occ->target,
        protocol::Range{*start, *end}
    };
}

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    MergedIndexShard::find_occurrence(std::uint32_t offset) const {
    auto* m = mapper();
    if(!m)
        return std::nullopt;
    std::optional<std::pair<index::SymbolHash, protocol::Range>> result;
    index.lookup(offset, [&](const index::Occurrence& o) {
        auto start = m->to_position(o.range.begin);
        auto end = m->to_position(o.range.end);
        if(start && end) {
            result = {
                o.target,
                protocol::Range{*start, *end}
            };
        }
        return false;
    });
    return result;
}

llvm::SmallVector<std::uint32_t> Workspace::on_file_saved(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> dirtied;

    // Re-scan the saved file for module declarations and update path_to_module.
    auto file_path = path_pool.resolve(path_id);
    if(auto buf = llvm::MemoryBuffer::getFile(file_path)) {
        auto result = scan((*buf)->getBuffer());
        if(!result.module_name.empty()) {
            path_to_module[path_id] = std::move(result.module_name);
        } else {
            path_to_module.erase(path_id);
        }
    }

    if(compile_graph) {
        auto result = compile_graph->update(path_id);
        for(auto id: result) {
            dirtied.push_back(id);
            // Invalidate active PCM state and its artifact cache entry.
            if(auto it = pcm_active.find(id); it != pcm_active.end()) {
                artifact_cache.invalidate(it->second);
                pcm_active.erase(it);
            }
        }
    }
    return dirtied;
}

void Workspace::on_file_closed(std::uint32_t path_id) {
    if(compile_graph && compile_graph->has_unit(path_id)) {
        compile_graph->update(path_id);
    }
    // Clear active PCH state.  The artifact remains in artifact_cache
    // for potential reuse by other files with the same preamble+flags.
    pch_active.erase(path_id);
}

void Workspace::load_cache() {
    artifact_cache.load(path_pool, config.cache_dir);
}

void Workspace::save_cache() {
    artifact_cache.save(path_pool, config.cache_dir);
}

void Workspace::cleanup_cache(int max_age_days) {
    ArtifactCache::cleanup(config.cache_dir, max_age_days);
}

void Workspace::build_module_map() {
    for(auto& [module_name, path_ids]: dep_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }
}

void Workspace::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                              std::uint32_t exclude_path_id) const {
    for(auto& [pid, artifact_key]: pcm_active) {
        if(pid == exclude_path_id)
            continue;
        auto mod_it = path_to_module.find(pid);
        if(mod_it == path_to_module.end())
            continue;
        // Look up the artifact to get its disk path.
        // const_cast is safe: lookup only updates last_access.
        auto* entry = const_cast<ArtifactCache&>(artifact_cache).lookup(artifact_key);
        if(entry) {
            pcms[mod_it->second] = entry->path;
        }
    }
}

void Workspace::cancel_all() {
    if(compile_graph) {
        compile_graph->cancel_all();
    }
}

}  // namespace clice
