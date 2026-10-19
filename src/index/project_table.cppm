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
/// file table's ids. The columns are read in place and their values
/// checked when read: an entry whose arena, id or kind is out of range
/// reads as empty, so a load touches only the pages its reads reach.
struct ProjectTable {
    using Blob = ProjectTableBlob;

    std::unique_ptr<llvm::MemoryBuffer> buffer;

    const archive::Archived<Blob>* columns = nullptr;

    IdColumnView parents;

    IdColumnView files;

    IdColumnView reference_counts;

    /// Blob path id -> file table id.
    std::vector<Fid> remap;

    /// File table id -> blob path id.
    llvm::DenseMap<Fid, std::uint32_t> path_index;

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

    llvm::StringRef name(std::uint32_t entry) const;

    llvm::StringRef arguments(std::uint32_t entry) const;

    /// 0 for a symbol with no parent.
    SymbolHash parent(std::uint32_t entry) const;

    /// The symbol's file as a path id of the blob, or no_file.
    std::uint32_t file(std::uint32_t entry) const;

    llvm::StringRef signature(std::uint32_t entry) const;

    llvm::ArrayRef<std::uint8_t> image(std::uint32_t entry) const;

    std::optional<Bitmap> bitmap(std::uint32_t entry) const {
        return read_file_set(image(entry));
    }

    /// Empty for a symbol no instantiation names.
    llvm::ArrayRef<std::uint8_t> instantiation_image(std::uint32_t entry) const;

    std::optional<Bitmap> instantiation_bitmap(std::uint32_t entry) const {
        return read_file_set(instantiation_image(entry));
    }

    /// The blob's path id of a file; nullopt for one its path table does
    /// not name.
    std::optional<std::uint32_t> path_id(Fid file) const {
        auto it = path_index.find(file);
        return it != path_index.end() ? std::optional(it->second) : std::nullopt;
    }

    /// A TU's unit, and what the units say of a file, a place, an input or
    /// a symbol; null when there is none.
    const PersistedUnit* unit(Fid tu) const;

    const archive::Archived<ContributedFile>* contributed(Fid file) const;

    const archive::Archived<UsedFile>* used(Fid file) const;

    const archive::Archived<UnitSet>* absent(Fid place) const;

    const archive::Archived<UnitSet>* imported(Fid input) const;

    const archive::Archived<SharedLocalEntry>* shared_local(SymbolHash symbol) const;

    SymbolView view(std::uint32_t entry) const;

    /// Bind the columns of a blob verify_root accepted, checking the sizes
    /// every read indexes by.
    std::expected<void, llvm::StringRef> bind(const archive::Archived<Blob>& root);

    /// Read the same bytes from `replacement`: another read snapshot's, or
    /// a copy. False when its size differs.
    bool rebind(std::unique_ptr<llvm::MemoryBuffer> replacement);

private:
    void point(const archive::Archived<Blob>& root);
};

}  // namespace clice::index
