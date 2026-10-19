module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_table;
import :semantic.token_class;

namespace clice::index {

/// Most file sets hold one or two files: up to raw_file_set_limit path ids
/// are stored raw as u32s, more as a portable roaring image — which is
/// longer than any raw list, so a slice's length says which it is.
constexpr static std::uint32_t raw_file_set_limit = 2;

void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids) {
    if(ids.cardinality() <= raw_file_set_limit) {
        for(std::uint32_t id: ids) {
            auto* bytes = reinterpret_cast<const std::uint8_t*>(&id);
            arena.insert(arena.end(), bytes, bytes + sizeof(id));
        }
        return;
    }
    auto image = write_bitmap(ids);
    auto* bytes = reinterpret_cast<const std::uint8_t*>(image.data());
    arena.insert(arena.end(), bytes, bytes + image.size());
}

std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes) {
    if(bytes.size() > raw_file_set_limit * sizeof(std::uint32_t)) {
        auto image = read_bitmap(bytes.data(), bytes.size());
        if(!image || image->cardinality() <= raw_file_set_limit) {
            return std::nullopt;
        }
        return image;
    }
    if(bytes.size() % sizeof(std::uint32_t) != 0) {
        return std::nullopt;
    }
    Bitmap ids;
    std::optional<std::uint32_t> previous;
    for(std::uint32_t i = 0; i < bytes.size(); i += sizeof(std::uint32_t)) {
        std::uint32_t id;
        std::memcpy(&id, bytes.data() + i, sizeof(id));
        if(previous && id <= *previous) {
            return std::nullopt;
        }
        ids.add(id);
        previous = id;
    }
    return ids;
}

/// The entries of key `position` of a keyed table whose runs end at `ends`.
static std::pair<std::uint32_t, std::uint32_t> run_of(llvm::ArrayRef<std::uint32_t> ends,
                                                      std::uint32_t position) {
    return {position == 0 ? 0 : ends[position - 1], ends[position]};
}

/// The unit set of key `id` in a table of keys ascending, one set each.
static std::optional<Bitmap> keyed_units(llvm::ArrayRef<std::uint32_t> keys,
                                         llvm::ArrayRef<std::uint32_t> ends,
                                         llvm::ArrayRef<std::uint8_t> units,
                                         std::optional<std::uint32_t> id) {
    auto position = id ? sorted_find(keys, *id) : std::nullopt;
    if(!position) {
        return std::nullopt;
    }
    return read_file_set(slice(units, ends, *position));
}

llvm::SmallVector<ProjectTable::Contribution, 2> ProjectTable::contributions_of(Fid file) const {
    llvm::SmallVector<Contribution, 2> found;
    auto id = path_id(file);
    auto position = id ? sorted_find(columns->contributed_files, *id) : std::nullopt;
    if(!position) {
        return found;
    }
    auto [begin, end] = run_of(columns->contribution_ends, *position);
    for(auto entry = begin; entry < end; entry += 1) {
        if(auto units = read_file_set(
               slice(columns->contribution_units, columns->contribution_unit_ends, entry))) {
            found.push_back({
                .variant = columns->contribution_variants[entry],
                .version = columns->contribution_versions[entry],
                .units = std::move(*units),
            });
        }
    }
    return found;
}

llvm::SmallVector<ProjectTable::Use, 2> ProjectTable::uses_of(Fid file) const {
    llvm::SmallVector<Use, 2> found;
    auto id = path_id(file);
    auto position = id ? sorted_find(columns->use_files, *id) : std::nullopt;
    if(!position) {
        return found;
    }
    auto [begin, end] = run_of(columns->use_ends, *position);
    for(auto entry = begin; entry < end; entry += 1) {
        if(auto units = read_file_set(slice(columns->use_units, columns->use_unit_ends, entry))) {
            found.push_back({.use = columns->use_hashes[entry], .units = std::move(*units)});
        }
    }
    return found;
}

std::optional<Bitmap> ProjectTable::absent_units(Fid place) const {
    return keyed_units(columns->absent_places,
                       columns->absent_unit_ends,
                       columns->absent_units,
                       path_id(place));
}

std::optional<Bitmap> ProjectTable::import_units(Fid input) const {
    return keyed_units(columns->import_inputs,
                       columns->import_unit_ends,
                       columns->import_units,
                       path_id(input));
}

llvm::SmallVector<ProjectTable::SharedLocalSet>
    ProjectTable::shared_local_sets_of(SymbolHash symbol) const {
    llvm::SmallVector<SharedLocalSet> sets;
    auto position = sorted_find(columns->shared_local_symbols, symbol);
    if(!position) {
        return sets;
    }
    auto [begin, end] = run_of(columns->shared_local_set_ends, *position);
    for(auto set = begin; set < end; set += 1) {
        auto files =
            read_file_set(slice(columns->shared_local_files, columns->shared_local_file_ends, set));
        auto units =
            read_file_set(slice(columns->shared_local_units, columns->shared_local_unit_ends, set));
        if(files && units) {
            sets.push_back({.files = std::move(*files), .units = std::move(*units)});
        }
    }
    return sets;
}

std::expected<void, llvm::StringRef> ProjectTable::bind(const archive::Archived<Blob>& root) {
    columns = &root;
    auto& paths = columns->paths;
    auto hashes = this->hashes();
    parents = ids_of(columns->symbol_parents, hashes.size() + 1);
    files = ids_of(columns->symbol_files, paths.size() + 1);
    reference_counts = ids_of(columns->symbol_reference_counts, paths.size() + 1);

    llvm::ArrayRef<std::uint32_t> version_ids = columns->version_ids;
    auto version_count = version_ids.size();
    if(columns->version_paths.size() != version_count ||
       columns->version_hashes.size() != version_count) {
        return std::unexpected("file version columns do not line up");
    }
    if(!std::ranges::is_sorted(version_ids)) {
        return std::unexpected("file version ids are not ascending");
    }
    if(columns->version_unit_ends.size() != version_count ||
       !covering_ends(columns->version_unit_ends, columns->version_units.size())) {
        return std::unexpected("file version units do not line up");
    }
    // A keyed table: keys ascending inside the path table, a run of
    // entries each, and a set of units per entry.
    auto keyed_ok = [&](llvm::ArrayRef<std::uint32_t> keys,
                        llvm::ArrayRef<std::uint32_t> runs,
                        std::uint32_t entries,
                        llvm::ArrayRef<std::uint32_t> unit_ends,
                        std::uint32_t unit_bytes) {
        return sparse_ok(keys, runs.size(), paths.size()) && covering_ends(runs, entries) &&
               unit_ends.size() == entries && covering_ends(unit_ends, unit_bytes);
    };
    auto variants = columns->contribution_variants.size();
    if(columns->contribution_versions.size() != variants ||
       !keyed_ok(columns->contributed_files,
                 columns->contribution_ends,
                 variants,
                 columns->contribution_unit_ends,
                 columns->contribution_units.size())) {
        return std::unexpected("contribution columns do not line up");
    }
    if(!keyed_ok(columns->use_files,
                 columns->use_ends,
                 columns->use_hashes.size(),
                 columns->use_unit_ends,
                 columns->use_units.size())) {
        return std::unexpected("instantiation use columns do not line up");
    }
    // One set of units per key.
    auto sets_ok = [&](llvm::ArrayRef<std::uint32_t> keys,
                       llvm::ArrayRef<std::uint32_t> unit_ends,
                       std::uint32_t unit_bytes) {
        return sparse_ok(keys, unit_ends.size(), paths.size()) &&
               covering_ends(unit_ends, unit_bytes);
    };
    if(!sets_ok(columns->absent_places, columns->absent_unit_ends, columns->absent_units.size()) ||
       !sets_ok(columns->import_inputs, columns->import_unit_ends, columns->import_units.size())) {
        return std::unexpected("absent place or import columns do not line up");
    }
    llvm::ArrayRef<std::uint64_t> shared_local_symbols = columns->shared_local_symbols;
    llvm::ArrayRef<std::uint32_t> shared_local_set_ends = columns->shared_local_set_ends;
    llvm::ArrayRef<std::uint32_t> shared_local_file_ends = columns->shared_local_file_ends;
    llvm::ArrayRef<std::uint32_t> shared_local_unit_ends = columns->shared_local_unit_ends;
    if(shared_local_set_ends.size() != shared_local_symbols.size() ||
       !covering_ends(shared_local_set_ends, shared_local_file_ends.size()) ||
       !covering_ends(shared_local_file_ends, columns->shared_local_files.size()) ||
       shared_local_unit_ends.size() != shared_local_file_ends.size() ||
       !covering_ends(shared_local_unit_ends, columns->shared_local_units.size()) ||
       !strictly_ascending(shared_local_symbols)) {
        return std::unexpected("shared local columns do not line up");
    }
    auto units = columns->manifest_versions.size();
    if(columns->manifest_generations.size() != units || columns->manifest_runs.size() != units ||
       !strictly_ascending(columns->manifest_versions)) {
        return std::unexpected("unit columns do not line up");
    }
    auto count = hashes.size();
    llvm::ArrayRef<std::uint8_t> token_kinds = columns->symbol_token_kinds;
    if(columns->symbol_name_ends.size() != count ||
       columns->symbol_signature_ends.size() != count || columns->symbol_kinds.size() != count ||
       columns->symbol_flags.size() != count || token_kinds.size() != count ||
       columns->symbol_token_modifiers.size() != count ||
       columns->symbol_bitmap_ends.size() != count) {
        return std::unexpected("symbol columns do not line up");
    }
    if(!all_token_kinds(token_kinds)) {
        return std::unexpected("a symbol's token kind is out of range");
    }
    if(!ids_ok(parents, count, count + 1) || !ids_ok(files, count, paths.size() + 1) ||
       !ids_ok(reference_counts, count, paths.size() + 1)) {
        return std::unexpected("symbol id columns do not line up");
    }
    llvm::ArrayRef<std::uint32_t> far_parents = columns->symbol_far_parents;
    llvm::ArrayRef<std::uint64_t> far_parent_hashes = columns->symbol_far_parent_hashes;
    auto sparse = [&](llvm::ArrayRef<std::uint32_t> entries, std::uint32_t values) {
        return sparse_ok(entries, values, count);
    };
    if(!sparse(columns->symbol_args_entries, columns->symbol_args_ends.size()) ||
       !sparse(columns->symbol_instantiation_entries, columns->symbol_instantiation_ends.size()) ||
       !sparse(far_parents, far_parent_hashes.size())) {
        return std::unexpected("sparse symbol columns do not line up");
    }
    if(!monotone_ends(columns->symbol_name_ends, columns->symbol_names.size()) ||
       !monotone_ends(columns->symbol_args_ends, columns->symbol_args.size()) ||
       !monotone_ends(columns->symbol_signature_ends, columns->symbol_signatures.size()) ||
       !monotone_ends(columns->symbol_bitmap_ends, columns->symbol_bitmaps.size()) ||
       !monotone_ends(columns->symbol_instantiation_ends, columns->symbol_instantiations.size())) {
        return std::unexpected("symbol arenas do not line up");
    }
    // Hashes, parents and pending entries become table keys downstream, which
    // reserve two sentinel values; a repeated hash would let one entry shadow
    // another.
    if(!strictly_ascending(hashes) || llvm::any_of(hashes, reserved_key<SymbolHash>)) {
        return std::unexpected("symbol hashes are not ascending");
    }
    for(std::uint32_t k = 0; k < far_parents.size(); k += 1) {
        if(parents[far_parents[k]] != 0 || reserved_key(far_parent_hashes[k])) {
            return std::unexpected("symbol parent outside the table is not canonical");
        }
    }
    for(auto hash: columns->search_pending) {
        if(reserved_key(hash)) {
            return std::unexpected("reserved pending search entry");
        }
    }
    for(auto file: columns->search_pending_files) {
        if(file >= paths.size()) {
            return std::unexpected("pending search file outside the path table");
        }
    }
    // The writer only emits interned paths, which are never empty; an
    // empty entry marks a corrupt blob and must not become a real pool
    // entry.
    for(std::uint32_t i = 0; i < paths.size(); i += 1) {
        if(paths[i].empty()) {
            return std::unexpected("empty symbol path");
        }
    }
    return {};
}

}  // namespace clice::index
