module;

#include "modules/prelude.h"

module clice:index.project_table;

import :index.columns;
import :index.format;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
import :support.archive;
import :support.bitmap;
import :vfs.ids;

namespace clice::index {

/// Append `ids` to `arena` as one file set: a slice of raw path ids, or a
/// portable roaring image when there are more than a few.
void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids);

/// The set a slice holds; nullopt when it is not one append_file_set writes.
std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes);

/// The bound project table: its columns, and its path table mapped to the
/// file table's ids.
struct ProjectTable {
    using Blob = ProjectTableBlob;

    std::unique_ptr<llvm::MemoryBuffer> buffer;

    const archive::Archived<Blob>* columns = nullptr;

    IdColumnView parents;

    IdColumnView files;

    IdColumnView reference_counts;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// File table id -> blob path id, built on first use.
    mutable llvm::DenseMap<Fid, std::uint32_t> path_index;

    llvm::ArrayRef<std::uint64_t> hashes() const {
        return columns->symbol_hashes;
    }

    const archive::Vec<archive::Vec<char>>& paths() const {
        return columns->paths;
    }

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(hashes().size());
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        return sorted_find(hashes(), hash);
    }

    llvm::StringRef name(std::uint32_t entry) const {
        return slice(columns->symbol_names, columns->symbol_name_ends, entry);
    }

    llvm::StringRef arguments(std::uint32_t entry) const {
        auto position = sorted_find(columns->symbol_args_entries, entry);
        if(!position) {
            return {};
        }
        return slice(columns->symbol_args, columns->symbol_args_ends, *position);
    }

    SymbolHash parent(std::uint32_t entry) const {
        if(parents[entry] != 0) {
            return hashes()[parents[entry] - 1];
        }
        auto position = sorted_find(columns->symbol_far_parents, entry);
        if(!position) {
            return 0;
        }
        return columns->symbol_far_parent_hashes[*position];
    }

    /// The symbol's file as a path id of the blob, or no_file.
    std::uint32_t file(std::uint32_t entry) const {
        return files[entry] == 0 ? no_file : files[entry] - 1;
    }

    llvm::StringRef signature(std::uint32_t entry) const {
        return slice(columns->symbol_signatures, columns->symbol_signature_ends, entry);
    }

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t entry) const {
        return slice(columns->symbol_bitmaps, columns->symbol_bitmap_ends, entry);
    }

    std::optional<Bitmap> bitmap(std::uint32_t entry) const {
        return read_file_set(image(entry));
    }

    /// Empty for a symbol no instantiation names.
    llvm::ArrayRef<std::uint8_t> instantiation_image(std::uint32_t entry) const {
        auto position = sorted_find(columns->symbol_instantiation_entries, entry);
        if(!position) {
            return {};
        }
        return slice(columns->symbol_instantiations, columns->symbol_instantiation_ends, *position);
    }

    std::optional<Bitmap> instantiation_bitmap(std::uint32_t entry) const {
        return read_file_set(instantiation_image(entry));
    }

    /// The blob's path id of a file; nullopt for one its path table does
    /// not name.
    std::optional<std::uint32_t> path_id(Fid file) const {
        if(path_index.empty()) {
            for(std::uint32_t id = 0; id < remap.size(); id += 1) {
                path_index.try_emplace(remap[id], id);
            }
        }
        auto it = path_index.find(file);
        return it != path_index.end() ? std::optional(it->second) : std::nullopt;
    }

    /// What the units say of a file, a place, an input or a symbol; null
    /// when they say nothing.
    const archive::Archived<ContributedFile>* contributed(Fid file) const;

    const archive::Archived<UsedFile>* used(Fid file) const;

    const archive::Archived<UnitSet>* absent(Fid place) const;

    const archive::Archived<UnitSet>* imported(Fid input) const;

    const archive::Archived<SharedLocalEntry>* shared_local(SymbolHash symbol) const;

    SymbolView view(std::uint32_t entry) const {
        auto file = this->file(entry);
        return {
            .name = name(entry),
            .args = arguments(entry),
            .signature = signature(entry),
            .parent = parent(entry),
            .kind = SymbolKind(columns->symbol_kinds[entry]),
            .scope = SymbolScope::External,
            .flags = static_cast<SymbolFlags>(columns->symbol_flags[entry]),
            .file = file == no_file ? no_file : remap[file].raw,
            .token_class = {.kind =
                                static_cast<SemanticTokenKind>(columns->symbol_token_kinds[entry]),
                            .modifiers = columns->symbol_token_modifiers[entry]},
        };
    }

    /// Bind the columns of a blob verify_root accepted, checking they line
    /// up.
    std::expected<void, llvm::StringRef> bind(const archive::Archived<Blob>& root);
};

}  // namespace clice::index
