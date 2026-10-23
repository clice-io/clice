module;

#include "support/prelude.h"

/// The records the project index keeps in the database besides shards,
/// instantiation shards and manifests, decoded: the header, a file's
/// record and its dependents, the internal names it declares, a symbol's
/// record, a shared local's and a file version's. Files are named by path
/// ids throughout and file versions by version ids, the database's own ids
/// (ProjectIndex maps them to the file table's).

module clice:index.project_records;

import :index.format;
import :index.symbol;
import :semantic.symbol;
import :support.bitmap;

namespace clice::index {

/// Append `ids` to `arena` as one file set: nothing for no id; else each
/// id's distance past the previous one as a varint, the first's past -1 —
/// or, when that is longer than it, a zero byte and the set's portable
/// roaring image.
void append_file_set(std::vector<std::uint8_t>& arena, const Bitmap& ids);

/// The set a slice holds; nullopt when it is not one append_file_set writes.
std::optional<Bitmap> read_file_set(llvm::ArrayRef<std::uint8_t> bytes);

/// HeaderBlob decoded.
struct Header {
    std::uint32_t next_path = 0;

    std::uint32_t next_version = 0;

    std::uint32_t unit_count = 0;

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

/// A version record: the file's path id and the content hash.
struct PersistedVersion {
    std::uint32_t path = 0;

    std::uint64_t content_hash = 0;

    friend bool operator==(const PersistedVersion&, const PersistedVersion&) = default;
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

/// A version of a file the manifests name: its content hash, its version
/// record's id and how many manifests name it.
struct NamedVersion {
    std::uint64_t content_hash = 0;

    std::uint32_t id = 0;

    std::uint32_t manifests = 0;

    friend bool operator==(const NamedVersion&, const NamedVersion&) = default;
};

/// DependentsBlob decoded.
struct Dependents {
    KeyedUnits readers;

    KeyedUnits contributors;

    Bitmap absent;

    KeyedUnits importers;

    KeyedUnits uses;

    std::vector<IncludeResolution> resolutions;

    /// Ascending by content hash.
    std::vector<NamedVersion> versions;

    bool empty() const {
        return readers.empty() && contributors.empty() && absent.isEmpty() && importers.empty() &&
               uses.empty() && resolutions.empty() && versions.empty();
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

/// A symbol's record, packed by hand: it is the most numerous record, and a
/// blob's header and placement pairs would double it. The name, the
/// arguments and the signature, each its size as a varint and its bytes;
/// the parent, 8 bytes little-endian; the canonical file's path id plus one
/// (0 for none), the token modifiers and the flags as varints; the kind and
/// the token kind, a byte each; the reference files' set, its size as a
/// varint and its bytes; the instantiation files' set to the end.
std::vector<std::byte> encode_record(const Symbol& symbol);

std::vector<std::byte> encode_record(const SharedLocalSets& local);

/// A version record, 12 bytes: the path id, 4 bytes little-endian, and the
/// content hash, 8.
std::vector<std::byte> encode_record(const PersistedVersion& version);

/// The record `data` holds, or nullopt when the bytes are not one: the
/// layout check and the checks every reader relies on. A header of another
/// format version is not one either.
std::optional<Header> decode_header(llvm::StringRef data);

std::optional<FileRecord> decode_file(llvm::StringRef data);

std::optional<Dependents> decode_dependents(llvm::StringRef data);

std::optional<Internals> decode_internals(llvm::StringRef data);

std::optional<Symbol> decode_symbol(llvm::StringRef data);

/// Add the parts of the symbol record `data` to `bytes`, as measure_blob
/// does a blob's fields; nothing for bytes that are no record.
void measure_symbol(llvm::StringRef data, FieldBytes& bytes);

std::optional<SharedLocalSets> decode_shared_local(llvm::StringRef data);

std::optional<PersistedVersion> decode_version(llvm::StringRef data);

}  // namespace clice::index
