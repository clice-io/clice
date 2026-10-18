module;

#include "modules/prelude.h"

module clice:index.project_table;

import :index.columns;
import :index.format;
import :index.symbol;
import :semantic.token_class;
import :support.bitmap;
import :vfs.ids;

namespace clice::index {

/// Append `ids` to `arena` as one file set: a slice of raw path ids, or a
/// portable roaring image when there are more than a few.
void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids);

/// The set a slice holds; nullopt when it is not one append_file_set writes.
std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes);

/// The bound global blob: its columns, and its path table mapped to the
/// file table's ids.
struct ProjectTable {
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    std::uint64_t generation = 0;
    std::uint32_t next_fv_id = 0;
    llvm::ArrayRef<std::uint32_t> fv_ids;
    kota::codec::fbs::array_view<std::string> fv_paths;
    llvm::ArrayRef<std::uint64_t> fv_hashes;

    kota::codec::fbs::array_view<std::string> paths;
    llvm::ArrayRef<std::uint64_t> hashes;
    llvm::StringRef names;
    llvm::ArrayRef<std::uint32_t> name_ends;
    llvm::ArrayRef<std::uint32_t> args_docs;
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
    llvm::ArrayRef<std::uint32_t> instantiation_docs;
    llvm::ArrayRef<std::uint32_t> instantiation_ends;
    llvm::ArrayRef<std::uint8_t> instantiation_images;

    llvm::ArrayRef<std::uint32_t> manifest_fvs;
    llvm::ArrayRef<std::uint64_t> manifest_gens;

    std::uint64_t search_generation = 0;
    llvm::ArrayRef<std::uint64_t> search_pending;
    llvm::ArrayRef<std::uint32_t> search_pending_files;

    llvm::ArrayRef<std::uint32_t> contributed_files;
    llvm::ArrayRef<std::uint32_t> contributor_ends;
    llvm::ArrayRef<std::uint8_t> contributors;

    llvm::ArrayRef<std::uint64_t> fanout_symbols;
    llvm::ArrayRef<std::uint32_t> fanout_set_ends;
    llvm::ArrayRef<std::uint32_t> fanout_file_ends;
    llvm::ArrayRef<std::uint8_t> fanout_files;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// File table id -> index into contributed_files, built on first use.
    mutable llvm::DenseMap<Fid, std::uint32_t> contributed_index;

    std::uint32_t count() const {
        return static_cast<std::uint32_t>(hashes.size());
    }

    std::optional<std::uint32_t> find(SymbolHash hash) const {
        auto it = std::ranges::lower_bound(hashes, hash);
        if(it == hashes.end() || *it != hash) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(it - hashes.begin());
    }

    llvm::StringRef name(std::uint32_t doc) const {
        return back_to_back(names, name_ends, doc);
    }

    llvm::StringRef arguments(std::uint32_t doc) const {
        auto it = std::ranges::lower_bound(args_docs, doc);
        if(it == args_docs.end() || *it != doc) {
            return {};
        }
        return back_to_back(args, args_ends, static_cast<std::uint32_t>(it - args_docs.begin()));
    }

    SymbolHash parent(std::uint32_t doc) const {
        if(parents[doc] != 0) {
            return hashes[parents[doc] - 1];
        }
        auto it = std::ranges::lower_bound(far_parents, doc);
        if(it == far_parents.end() || *it != doc) {
            return 0;
        }
        return far_parent_hashes[it - far_parents.begin()];
    }

    /// The symbol's file as a path id of the blob, or no_file.
    std::uint32_t file(std::uint32_t doc) const {
        return files[doc] == 0 ? no_file : files[doc] - 1;
    }

    llvm::StringRef signature(std::uint32_t doc) const {
        return back_to_back(signatures, signature_ends, doc);
    }

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t doc) const {
        return back_to_back(bitmaps, bitmap_ends, doc);
    }

    std::optional<Bitmap> bitmap(std::uint32_t doc) const {
        return read_file_set(image(doc));
    }

    /// Empty for a symbol no instantiation names.
    llvm::ArrayRef<std::uint8_t> instantiation_image(std::uint32_t doc) const {
        auto it = std::ranges::lower_bound(instantiation_docs, doc);
        if(it == instantiation_docs.end() || *it != doc) {
            return {};
        }
        return back_to_back(instantiation_images,
                            instantiation_ends,
                            static_cast<std::uint32_t>(it - instantiation_docs.begin()));
    }

    std::optional<Bitmap> instantiation_bitmap(std::uint32_t doc) const {
        return read_file_set(instantiation_image(doc));
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
        return read_file_set(back_to_back(contributors, contributor_ends, it->second));
    }

    /// The file sets the local fanouts list for `symbol`, as path ids; a
    /// corrupt one is left out.
    llvm::SmallVector<Bitmap> fanout_sets_of(SymbolHash symbol) const {
        llvm::SmallVector<Bitmap> sets;
        auto it = std::ranges::lower_bound(fanout_symbols, symbol);
        if(it == fanout_symbols.end() || *it != symbol) {
            return sets;
        }
        auto entry = static_cast<std::uint32_t>(it - fanout_symbols.begin());
        for(auto set = entry == 0 ? 0 : fanout_set_ends[entry - 1]; set < fanout_set_ends[entry];
            set += 1) {
            if(auto files = read_file_set(back_to_back(fanout_files, fanout_file_ends, set))) {
                sets.push_back(std::move(*files));
            }
        }
        return sets;
    }

    SymbolView view(std::uint32_t doc) const {
        auto file = this->file(doc);
        return {
            .name = name(doc),
            .args = arguments(doc),
            .signature = signature(doc),
            .parent = parent(doc),
            .kind = SymbolKind(kinds[doc]),
            .scope = SymbolScope::External,
            .flags = static_cast<SymbolFlags>(flags[doc]),
            .file = file == no_file ? no_file : remap[file].raw,
            .token_class = {.kind = static_cast<SemanticTokenKind>(token_kinds[doc]),
                            .modifiers = token_modifiers[doc]},
        };
    }

    /// Bind the columns of a verified blob, checking they line up.
    std::expected<void, llvm::StringRef> bind(kota::codec::fbs::table_view<GlobalBlob> root);
};

/// Add the fields of a global blob to `bytes`, for `clice index stats`.
void measure_global(llvm::StringRef data, FieldBytes& bytes);

}  // namespace clice::index
