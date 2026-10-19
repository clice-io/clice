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

/// The record of `key` among records ascending by it; null for none.
template <typename Record, typename Key>
const static Record* find_record(const archive::Vec<Record>& records,
                                 Key key,
                                 Key Record::* field) {
    auto it = std::ranges::lower_bound(records, key, {}, field);
    return it != records.end() && (*it).*field == key ? &*it : nullptr;
}

const archive::Archived<ContributedFile>* ProjectTable::contributed(Fid file) const {
    auto id = path_id(file);
    return id ? find_record(columns->contributions, *id, &archive::Archived<ContributedFile>::file)
              : nullptr;
}

const archive::Archived<UsedFile>* ProjectTable::used(Fid file) const {
    auto id = path_id(file);
    return id ? find_record(columns->uses, *id, &archive::Archived<UsedFile>::file) : nullptr;
}

const archive::Archived<UnitSet>* ProjectTable::absent(Fid place) const {
    auto id = path_id(place);
    return id ? find_record(columns->absent, std::uint64_t(*id), &archive::Archived<UnitSet>::key)
              : nullptr;
}

const archive::Archived<UnitSet>* ProjectTable::imported(Fid input) const {
    auto id = path_id(input);
    return id ? find_record(columns->imports, std::uint64_t(*id), &archive::Archived<UnitSet>::key)
              : nullptr;
}

const archive::Archived<SharedLocalEntry>* ProjectTable::shared_local(SymbolHash symbol) const {
    return find_record(columns->shared_locals,
                       symbol,
                       &archive::Archived<SharedLocalEntry>::symbol);
}

std::expected<void, llvm::StringRef> ProjectTable::bind(const archive::Archived<Blob>& root) {
    columns = &root;
    auto& paths = columns->paths;
    auto hashes = this->hashes();
    parents = ids_of(columns->symbol_parents, hashes.size() + 1);
    files = ids_of(columns->symbol_files, paths.size() + 1);
    reference_counts = ids_of(columns->symbol_reference_counts, paths.size() + 1);

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
