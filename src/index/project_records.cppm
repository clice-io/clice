module;

#include "support/prelude.h"

/// The records the project index keeps in the database besides shards,
/// instantiation shards and manifests, decoded: the header, a file's
/// record and its dependents, the internal names it declares, a symbol's
/// record and a shared local's. Files are named by path ids throughout,
/// the database's own ids (ProjectIndex maps them to the file table's).

module clice:index.project_records;

import :index.format;
import :index.symbol;
import :semantic.symbol;
import :support.bitmap;

namespace clice::index {

/// Append `ids` to `arena` as one file set: a slice of raw path ids, or a
/// portable roaring image when there are more than a few.
void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids);

/// The set a slice holds; nullopt when it is not one append_file_set writes.
std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes);

/// HeaderBlob decoded.
struct Header {
    std::uint32_t next_path = 0;

    std::uint64_t search_generation = 0;

    std::vector<SymbolHash> search_pending;

    std::vector<std::uint32_t> search_pending_files;

    std::vector<std::uint32_t> owed_units;
};

/// FileBlob decoded.
struct FileRecord {
    std::uint32_t path = 0;

    bool absent = false;

    std::vector<std::uint64_t> consumed;

    std::uint64_t generation = 0;

    std::vector<VariantHash> stored;

    std::uint64_t run = 0;

    friend bool operator==(const FileRecord&, const FileRecord&) = default;
};

/// Units (path ids) under a key — a content hash, a variant, a use —
/// ascending by key, no key without units.
using KeyedUnits = std::vector<std::pair<std::uint64_t, Bitmap>>;

void add_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit);

void remove_unit(KeyedUnits& sets, std::uint64_t key, std::uint32_t unit);

/// How some units resolved a file's own `#include` lines at one content.
struct IncludeResolution {
    std::uint64_t content = 0;

    /// (line, target path id), in line order.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;

    Bitmap units;
};

/// DependentsBlob decoded.
struct Dependents {
    KeyedUnits readers;

    KeyedUnits contributors;

    Bitmap absent;

    KeyedUnits importers;

    KeyedUnits uses;

    std::vector<IncludeResolution> resolutions;

    bool empty() const {
        return readers.empty() && contributors.empty() && absent.isEmpty() && importers.empty() &&
               uses.empty() && resolutions.empty();
    }
};

/// One name of internal linkage a file declares.
struct InternalName {
    SymbolHash hash = 0;

    std::string name;

    std::string args;

    SymbolHash parent = 0;

    SymbolKind kind;

    SymbolFlags flags = SymbolFlags::None;
};

/// InternalsBlob decoded, ascending by hash.
using Internals = std::vector<InternalName>;

/// SharedLocalBlob decoded: each listed file set (path ids, ascending) and
/// how many units list it, ascending by set.
using SharedLocalSets = std::vector<std::pair<std::vector<std::uint32_t>, std::uint32_t>>;

std::vector<std::byte> encode_record(const Header& header);

std::vector<std::byte> encode_record(const FileRecord& file);

std::vector<std::byte> encode_record(const Dependents& dependents);

std::vector<std::byte> encode_record(const Internals& internals);

/// A symbol's record; its file and file sets are path ids.
std::vector<std::byte> encode_record(const Symbol& symbol);

std::vector<std::byte> encode_record(const SharedLocalSets& local);

/// The record `data` holds, or nullopt when the bytes are not one: the
/// layout check and the checks every reader relies on. A header of another
/// format version is not one either.
std::optional<Header> decode_header(llvm::StringRef data);

std::optional<FileRecord> decode_file(llvm::StringRef data);

std::optional<Dependents> decode_dependents(llvm::StringRef data);

std::optional<Internals> decode_internals(llvm::StringRef data);

std::optional<Symbol> decode_symbol(llvm::StringRef data);

std::optional<SharedLocalSets> decode_shared_local(llvm::StringRef data);

}  // namespace clice::index
