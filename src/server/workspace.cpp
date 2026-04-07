#include "server/workspace.h"

#include <algorithm>

#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"

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

// -- OpenFileIndex ------------------------------------------------------------

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

// -- MergedIndexShard ---------------------------------------------------------

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

// -- Workspace ----------------------------------------------------------------

llvm::SmallVector<std::uint32_t> Workspace::on_file_saved(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> dirtied;
    if(compile_graph) {
        auto result = compile_graph->update(path_id);
        for(auto id: result) {
            dirtied.push_back(id);
            pcm_paths.erase(id);
            pcm_cache.erase(id);
        }
    }
    return dirtied;
}

void Workspace::on_file_closed(std::uint32_t path_id) {
    if(compile_graph && compile_graph->has_unit(path_id)) {
        compile_graph->update(path_id);
    }
    pch_cache.erase(path_id);
}

}  // namespace clice
