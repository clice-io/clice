module;

#include "modules/prelude.h"

module clice;

import :index.columns;
import :index.format;
import :index.project_table;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :support.archive;

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

const PersistedUnit* ProjectTable::unit(Fid tu) const {
    auto id = path_id(tu);
    return id ? find_record(columns->units, *id, &PersistedUnit::file) : nullptr;
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

/// Entry `i` of an arena and its ends; empty when they do not line up.
template <typename Arena>
static Arena checked_slice(Arena arena, llvm::ArrayRef<std::uint32_t> ends, std::uint32_t i) {
    auto begin = i == 0 ? 0 : ends[i - 1];
    return begin <= ends[i] && ends[i] <= arena.size() ? slice_arena(arena, ends, i) : Arena();
}

llvm::StringRef ProjectTable::name(std::uint32_t entry) const {
    return checked_slice(llvm::StringRef(columns->symbol_names), columns->symbol_name_ends, entry);
}

llvm::StringRef ProjectTable::arguments(std::uint32_t entry) const {
    auto position = sorted_find(columns->symbol_args_entries, entry);
    if(!position) {
        return {};
    }
    return checked_slice(llvm::StringRef(columns->symbol_args),
                         columns->symbol_args_ends,
                         *position);
}

SymbolHash ProjectTable::parent(std::uint32_t entry) const {
    SymbolHash hash = 0;
    if(auto id = parents[entry]; id != 0) {
        hash = id <= count() ? hashes()[id - 1] : 0;
    } else if(auto position = sorted_find(columns->symbol_far_parents, entry)) {
        hash = columns->symbol_far_parent_hashes[*position];
    }
    // A hash becomes a table key downstream, which reserves two values.
    return reserved_key(hash) ? 0 : hash;
}

std::uint32_t ProjectTable::file(std::uint32_t entry) const {
    auto id = files[entry];
    return id == 0 || id > paths().size() ? no_file : id - 1;
}

llvm::StringRef ProjectTable::signature(std::uint32_t entry) const {
    return checked_slice(llvm::StringRef(columns->symbol_signatures),
                         columns->symbol_signature_ends,
                         entry);
}

llvm::ArrayRef<std::uint8_t> ProjectTable::image(std::uint32_t entry) const {
    return checked_slice(llvm::ArrayRef<std::uint8_t>(columns->symbol_bitmaps),
                         columns->symbol_bitmap_ends,
                         entry);
}

llvm::ArrayRef<std::uint8_t> ProjectTable::instantiation_image(std::uint32_t entry) const {
    auto position = sorted_find(columns->symbol_instantiation_entries, entry);
    if(!position) {
        return {};
    }
    return checked_slice(llvm::ArrayRef<std::uint8_t>(columns->symbol_instantiations),
                         columns->symbol_instantiation_ends,
                         *position);
}

SymbolView ProjectTable::view(std::uint32_t entry) const {
    auto file = this->file(entry);
    auto kind = columns->symbol_token_kinds[entry];
    return {
        .name = name(entry),
        .args = arguments(entry),
        .signature = signature(entry),
        .parent = parent(entry),
        .kind = SymbolKind(columns->symbol_kinds[entry]),
        .scope = SymbolScope::External,
        .flags = static_cast<SymbolFlags>(columns->symbol_flags[entry]),
        .file = file == no_file ? no_file : remap[file].raw,
        .token_class = {.kind = kind <= static_cast<std::uint8_t>(SemanticTokenKind::Plain)
                                    ? static_cast<SemanticTokenKind>(kind)
                                    : SemanticTokenKind::Plain,
                        .modifiers = columns->symbol_token_modifiers[entry]},
    };
}

void ProjectTable::point(const archive::Archived<Blob>& root) {
    columns = &root;
    parents = ids_of(columns->symbol_parents, count() + 1);
    files = ids_of(columns->symbol_files, paths().size() + 1);
    reference_counts = ids_of(columns->symbol_reference_counts, paths().size() + 1);
}

std::expected<void, llvm::StringRef> ProjectTable::bind(const archive::Archived<Blob>& root) {
    point(root);
    auto& paths = columns->paths;
    auto count = this->count();
    if(columns->symbol_name_ends.size() != count ||
       columns->symbol_signature_ends.size() != count || columns->symbol_kinds.size() != count ||
       columns->symbol_flags.size() != count || columns->symbol_token_kinds.size() != count ||
       columns->symbol_token_modifiers.size() != count ||
       columns->symbol_bitmap_ends.size() != count || parents.size() != count ||
       files.size() != count || reference_counts.size() != count) {
        return std::unexpected("symbol columns do not line up");
    }
    if(columns->symbol_args_entries.size() != columns->symbol_args_ends.size() ||
       columns->symbol_instantiation_entries.size() != columns->symbol_instantiation_ends.size() ||
       columns->symbol_far_parents.size() != columns->symbol_far_parent_hashes.size()) {
        return std::unexpected("sparse symbol columns do not line up");
    }
    // Pending entries become table keys, which reserve two values.
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
    // The units' records are found by binary search, and their path ids
    // index the path table.
    auto keyed = [&](const auto& records, auto key) {
        std::optional<std::uint64_t> previous;
        for(auto& record: records) {
            std::uint64_t id = record.*key;
            if(id >= paths.size() || (previous && id <= *previous)) {
                return false;
            }
            previous = id;
        }
        return true;
    };
    if(!keyed(columns->units, &PersistedUnit::file) ||
       !keyed(columns->contributions, &archive::Archived<ContributedFile>::file) ||
       !keyed(columns->uses, &archive::Archived<UsedFile>::file) ||
       !keyed(columns->absent, &archive::Archived<UnitSet>::key) ||
       !keyed(columns->imports, &archive::Archived<UnitSet>::key)) {
        return std::unexpected("a unit record names a file outside the path table");
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

bool ProjectTable::rebind(std::unique_ptr<llvm::MemoryBuffer> replacement) {
    if(!rebind_buffer(buffer, in_place_buffer(std::move(replacement)))) {
        return false;
    }
    point(archive::view<Blob>(buffer->getBuffer()));
    return true;
}

}  // namespace clice::index
