module;

#include "modules/prelude.h"

module clice:index.instantiation_shard;

import :index.types;

namespace clice::index {

/// What an instantiation record says about the code at its range.
enum class InstantiationKind : std::uint8_t {
    /// The name refers to the target: an occurrence of it with a Reference
    /// relation.
    Reference,
    /// The `auto` or `decltype` keyword stands for the target: an
    /// occurrence of it with a Deduced relation.
    Deduced,
    /// The keyword leads to the target (`new` to the constructor it runs):
    /// an occurrence of it with an Implied relation.
    Implied,
    /// `from` calls the target here: the Callee and Caller relations.
    Call,
    /// `from` derives from the target, the base specifier written here: an
    /// occurrence and a Reference of the target, the Base and Derived
    /// relations.
    Base,
    /// `from`, a variable or a parameter declared here with a placeholder
    /// type (`auto`), is of the target's type: a TypeDefinition relation.
    TypeDefinition,
};

/// Whether records of `kind` carry a `from`.
constexpr bool has_from(InstantiationKind kind) {
    return kind == InstantiationKind::Call || kind == InstantiationKind::Base ||
           kind == InstantiationKind::TypeDefinition;
}

/// One thing an instantiation resolves at a place of the template's text.
struct InstantiationRecord {
    LocalSourceRange range;
    InstantiationKind kind = InstantiationKind::Reference;
    SymbolHash target = 0;
    /// The caller of a call, the derived class of a base, the declaration
    /// of a type definition; 0 for the other kinds.
    SymbolHash from = 0;
    /// Whether a macro expansion resolves it, not an instantiation.
    bool expansion = false;

    friend bool operator==(const InstantiationRecord&, const InstantiationRecord&) = default;
};

/// The order records are stored in.
constexpr auto record_key(const InstantiationRecord& record) {
    return std::tuple(record.range.begin,
                      record.range.end,
                      static_cast<std::uint8_t>(record.kind),
                      record.expansion,
                      record.target,
                      record.from);
}

/// The identity of one resolution — the records one instantiated definition or
/// macro expansion resolves in one file: a hash over the file's content
/// generation and the records, which must be sorted and unique.
using ResolutionHash = std::uint64_t;

ResolutionHash resolution_identity(std::uint64_t content_hash,
                                   llvm::ArrayRef<InstantiationRecord> records);

/// The identity of the resolutions a TU uses in one file — a hash over them,
/// which must be sorted and unique. TUs using the same ones share it, and the
/// file's blob stores them once.
using UseHash = std::uint64_t;

UseHash use_identity(llvm::ArrayRef<ResolutionHash> resolutions);

/// What one run resolves in one file, as the worker builds it: the distinct
/// resolutions, and the keys it walked, each pointing at its resolution or at
/// none.
struct FileInstantiations {
    /// Each resolution's records, in any order; duplicates fold.
    std::vector<std::vector<InstantiationRecord>> resolutions;

    struct Key {
        /// The entity of the instantiated declaration.
        SymbolHash key = 0;
        /// The template it instantiates.
        SymbolHash pattern = 0;
        /// An index into `resolutions`, none when the instantiation resolves
        /// nothing in the file.
        std::optional<std::uint32_t> resolution;
    };

    std::vector<Key> keys;

    bool empty() const {
        return resolutions.empty() && keys.empty();
    }
};

/// The run that walked a key: the TU, by the hash of its database key
/// (ProjectIndex::key_of), and its manifest's built_at.
struct Producer {
    std::uint64_t unit = 0;
    std::uint64_t run = 0;

    friend bool operator==(const Producer&, const Producer&) = default;
};

/// Zero-copy reader over an instantiation blob, plus the live-resolution set
/// the indexer maintains: a resolution no live use holds any more stops
/// serving at once, and the next save's compaction erases it.
class InstantiationShard {
public:
    InstantiationShard() = default;

    /// Wrap verified blob bytes without owning them. Corrupt bytes or
    /// another format version load as an empty shard.
    static InstantiationShard from_bytes(llvm::StringRef data);

    /// Adopt an owning buffer of blob bytes; a null buffer, corrupt bytes
    /// or another format version load as an empty shard.
    static InstantiationShard from_buffer(std::unique_ptr<llvm::MemoryBuffer> buffer);

    /// Swap the backing buffer for byte-identical storage, as Shard::rebind.
    bool rebind(std::unique_ptr<llvm::MemoryBuffer> replacement);

    bool loaded() const {
        return buffer != nullptr;
    }

    llvm::StringRef bytes() const {
        return buffer ? buffer->getBuffer() : llvm::StringRef();
    }

    /// The content generation of the file the records lie in.
    std::uint64_t content_hash() const;

    /// Every stored resolution, ascending.
    llvm::ArrayRef<ResolutionHash> resolutions() const;

    /// Every stored use, ascending.
    llvm::ArrayRef<UseHash> uses() const;

    bool has_use(UseHash use) const;

    /// The resolutions of a stored use, ascending.
    std::vector<ResolutionHash> resolutions_of(UseHash use) const;

    /// Restrict queries to the resolutions of the given uses (the ones some
    /// manifest names); uses the blob does not store are ignored.
    void set_live(llvm::ArrayRef<UseHash> live);

    /// Whether set_live masked out a stored use or resolution — the signal
    /// that the next save should compact the blob.
    bool has_dead_uses() const;

    struct Key {
        SymbolHash key = 0;
        SymbolHash pattern = 0;
        /// None when the instantiation resolves nothing in the file.
        std::optional<ResolutionHash> resolution;
        /// None in a worker's blob.
        std::optional<Producer> producer;
    };

    /// Every key, ascending.
    llvm::ArrayRef<SymbolHash> keys() const;

    std::optional<Key> find_key(SymbolHash key) const;

    void for_each_key(llvm::function_ref<bool(const Key&)> callback) const;

    /// The live records whose range touches `offset`, in stored order.
    void lookup(std::uint32_t offset,
                llvm::function_ref<bool(const InstantiationRecord&)> callback) const;

    /// The live records naming `symbol` as their target or their `from`.
    void lookup(SymbolHash symbol,
                llvm::function_ref<bool(const InstantiationRecord&)> callback) const;

    /// Every live record, in stored order.
    void for_each_record(llvm::function_ref<bool(const InstantiationRecord&)> callback) const;

private:
    explicit InstantiationShard(std::unique_ptr<llvm::MemoryBuffer> buffer) :
        buffer(std::move(buffer)) {}

    bool live(std::uint32_t record) const {
        return live_records.empty() || ((live_records[record / 64] >> (record % 64)) & 1) != 0;
    }

    std::unique_ptr<llvm::MemoryBuffer> buffer;

    /// A bit per record some live resolution holds; empty while every
    /// stored use and resolution is live.
    std::vector<std::uint64_t> live_records;

    /// The length of the longest record, which bounds lookup(offset)'s scan.
    std::uint32_t longest = 0;
};

/// Encode one run's instantiations of one file as a worker blob: `content_hash`
/// and `content_size` describe the text the run compiled.
void write_instantiations(const FileInstantiations& instantiations,
                          std::uint64_t content_hash,
                          std::uint32_t content_size,
                          llvm::raw_ostream& os);

/// A use a merge adds: its identity and its resolutions, ascending, each one
/// stored in the merged blob.
struct AddedUse {
    UseHash use = 0;
    llvm::ArrayRef<ResolutionHash> resolutions;
};

/// Merge into one blob of the same content generation: the uses of `old` in
/// `keep` with their resolutions, the keys of `old` whose producer
/// `keep_producer` accepts and whose resolution survives, then `fresh`'s
/// resolutions and keys, walked by `producer` — a fresh key replaces a
/// stored one — and the `added` use. `old` may be empty (a first run) and
/// `fresh` null (a compaction, or a TU adding the use of what others
/// walked). Writes nothing and returns false when nothing survives: the
/// caller retires the file.
bool merge_instantiations(const InstantiationShard& old,
                          llvm::ArrayRef<UseHash> keep,
                          llvm::function_ref<bool(const Producer&)> keep_producer,
                          const InstantiationShard* fresh,
                          Producer producer,
                          std::optional<AddedUse> added,
                          llvm::raw_ostream& os);

}  // namespace clice::index
