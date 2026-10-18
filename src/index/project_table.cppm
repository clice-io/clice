module;

#include "modules/prelude.h"

module clice:index.project_table;

import :index.columns;
import :index.format;
import :index.symbol;
import :semantic.symbol;
import :semantic.token_class;
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
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    std::uint64_t generation = 0;
    std::uint32_t next_version_id = 0;
    llvm::ArrayRef<std::uint32_t> version_ids;
    kota::codec::fbs::array_view<std::string> version_paths;
    llvm::ArrayRef<std::uint64_t> version_hashes;

    kota::codec::fbs::array_view<std::string> paths;
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::ArrayRef<std::uint32_t> args_entries;
    llvm::StringRef args;
    llvm::ArrayRef<std::uint32_t> args_ends;
    llvm::StringRef signatures;
    llvm::ArrayRef<std::uint32_t> signature_ends;
    IdColumnView parents;
    llvm::ArrayRef<std::uint32_t> far_parents;
    llvm::ArrayRef<std::uint64_t> far_parent_hashes;
    llvm::ArrayRef<std::uint8_t> kinds;
    llvm::ArrayRef<std::uint16_t> flags;
    llvm::ArrayRef<std::uint8_t> token_kinds;
    llvm::ArrayRef<std::uint32_t> token_modifiers;
    IdColumnView files;
    IdColumnView reference_counts;
    llvm::ArrayRef<std::uint32_t> bitmap_ends;
    llvm::ArrayRef<std::uint8_t> bitmaps;
    llvm::ArrayRef<std::uint32_t> instantiation_entries;
    llvm::ArrayRef<std::uint32_t> instantiation_ends;
    llvm::ArrayRef<std::uint8_t> instantiation_images;

    llvm::ArrayRef<std::uint32_t> manifest_versions;
    llvm::ArrayRef<std::uint64_t> manifest_generations;

    std::uint64_t search_generation = 0;
    llvm::ArrayRef<std::uint64_t> search_pending;
    llvm::ArrayRef<std::uint32_t> search_pending_files;

    llvm::ArrayRef<std::uint32_t> contributed_files;
    llvm::ArrayRef<std::uint32_t> contributor_ends;
    llvm::ArrayRef<std::uint8_t> contributors;

    llvm::ArrayRef<std::uint64_t> shared_local_symbols;
    llvm::ArrayRef<std::uint32_t> shared_local_set_ends;
    llvm::ArrayRef<std::uint32_t> shared_local_file_ends;
    llvm::ArrayRef<std::uint8_t> shared_local_files;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// File table id -> index into contributed_files, built on first use.
    mutable llvm::DenseMap<Fid, std::uint32_t> contributed_index;

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(hashes.size());
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        return sorted_find(hashes, hash);
    }

    llvm::StringRef name(std::uint32_t entry) const {
        return slice(names, name_ends, entry);
    }

    llvm::StringRef arguments(std::uint32_t entry) const {
        auto position = sorted_find(args_entries, entry);
        if(!position) {
            return {};
        }
        return slice(args, args_ends, *position);
    }

    SymbolHash parent(std::uint32_t entry) const {
        if(parents[entry] != 0) {
            return hashes[parents[entry] - 1];
        }
        auto position = sorted_find(far_parents, entry);
        if(!position) {
            return 0;
        }
        return far_parent_hashes[*position];
    }

    /// The symbol's file as a path id of the blob, or no_file.
    std::uint32_t file(std::uint32_t entry) const {
        return files[entry] == 0 ? no_file : files[entry] - 1;
    }

    llvm::StringRef signature(std::uint32_t entry) const {
        return slice(signatures, signature_ends, entry);
    }

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t entry) const {
        return slice(bitmaps, bitmap_ends, entry);
    }

    std::optional<Bitmap> bitmap(std::uint32_t entry) const {
        return read_file_set(image(entry));
    }

    /// Empty for a symbol no instantiation names.
    llvm::ArrayRef<std::uint8_t> instantiation_image(std::uint32_t entry) const {
        auto position = sorted_find(instantiation_entries, entry);
        if(!position) {
            return {};
        }
        return slice(instantiation_images, instantiation_ends, *position);
    }

    std::optional<Bitmap> instantiation_bitmap(std::uint32_t entry) const {
        return read_file_set(instantiation_image(entry));
    }

    /// The TUs that contributed a variant to `file`, as path ids; nullopt when
    /// none did or the image fails to decode.
    std::optional<Bitmap> contributors_of(Fid file) const {
        if(contributed_index.empty()) {
            for(std::uint32_t i = 0; i < contributed_files.size(); i += 1) {
                contributed_index.try_emplace(remap[contributed_files[i]], i);
            }
        }
        auto it = contributed_index.find(file);
        if(it == contributed_index.end()) {
            return std::nullopt;
        }
        return read_file_set(slice(contributors, contributor_ends, it->second));
    }

    /// The file sets the shared locals list for `symbol`, as path ids; a
    /// corrupt one is left out.
    llvm::SmallVector<Bitmap> shared_local_sets_of(SymbolHash symbol) const {
        llvm::SmallVector<Bitmap> sets;
        auto found = sorted_find(shared_local_symbols, symbol);
        if(!found) {
            return sets;
        }
        auto position = *found;
        for(auto set = position == 0 ? 0 : shared_local_set_ends[position - 1];
            set < shared_local_set_ends[position];
            set += 1) {
            if(auto files = read_file_set(slice(shared_local_files, shared_local_file_ends, set))) {
                sets.push_back(std::move(*files));
            }
        }
        return sets;
    }

    SymbolView view(std::uint32_t entry) const {
        auto file = this->file(entry);
        return {
            .name = name(entry),
            .args = arguments(entry),
            .signature = signature(entry),
            .parent = parent(entry),
            .kind = SymbolKind(kinds[entry]),
            .scope = SymbolScope::External,
            .flags = static_cast<SymbolFlags>(flags[entry]),
            .file = file == no_file ? no_file : remap[file].raw,
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[entry]),
                            .modifiers = token_modifiers[entry]},
        };
    }

    /// Bind the columns of a blob verify_root accepted, checking they line
    /// up.
    std::expected<void, llvm::StringRef> bind(kota::codec::fbs::table_view<ProjectTableBlob> root);
};

}  // namespace clice::index
