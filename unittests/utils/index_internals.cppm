module clice:unittests.utils.index_internals;

import :index.database;
import :index.indexing.tu_index;
import :index.project_index;
import :index.project_index.records;
import :index.shard;
import :index.symbol;
import :support.prelude;
import :support.text;
import :vfs.ids;

namespace clice::testing {

/// A loaded shard whose entries were built from `content`, for the
/// disk-vs-shard freshness comparisons.
inline index::Shard shard_of(llvm::StringRef content) {
    auto bytes = index::write_shard(
        {},
        [](index::SymbolHash) -> std::optional<index::SymbolView> { return std::nullopt; },
        content);
    return index::Shard::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(as_chars(bytes)));
}

/// Index state a test sets up or looks at by hand, past the public API.
struct IndexInternals {
    /// Install `shard` as `file`'s: as a merge would, which the symbol
    /// records follow and the next save writes, when `written`; else as one
    /// read from the database.
    static void plant_shard(index::ProjectIndex& index,
                            Fid file,
                            index::Shard shard,
                            bool written = false) {
        if(written) {
            index.replace_shard(file, std::move(shard));
            index.drop_fileless_symbols();
            return;
        }
        auto& record = index.change_file(file);
        record.generation = shard.content_hash();
        record.stored = shard.variants();
        index.shard_records.cached[index.file_key(file)] = std::move(shard);
    }

    /// Retire `file`'s shard, as a compaction finding no live variant does.
    static void retire_shard(index::ProjectIndex& index, Fid file) {
        index.replace_shard(file, {});
        index.drop_fileless_symbols();
    }

    /// The record of the external symbol `hash`, to set up by hand.
    static index::Symbol& symbol(index::ProjectIndex& index, index::SymbolHash hash) {
        return index.change(index.symbol_records, index::number_key(hash), index::decode_symbol);
    }

    /// Merge what `tu_index` says its external symbols are, as a merge does
    /// before the shards say which files hold their entries.
    static void merge_facts(index::ProjectIndex& index, const index::TUIndex& tu_index) {
        llvm::SmallVector<index::SymbolHash> described;
        index.merge_facts(tu_index, described);
    }

    /// Whether `tu` is outdated (ProjectIndex::mark_outdated).
    static bool outdated(const index::ProjectIndex& index, Fid tu) {
        return index.outdated.contains(tu);
    }

    /// Mark `tu` outdated since `when` (ms since the epoch), as
    /// mark_outdated does at the present.
    static void outdated_since(index::ProjectIndex& index, Fid tu, std::int64_t when) {
        index.outdated[tu] = when;
    }

    /// Owe `tu` a reindex, as a rebuild discarding the variants it
    /// contributed does (ProjectIndex::owed_units).
    static void owe(index::ProjectIndex& index, Fid tu) {
        index::IndexReport report;
        index.owe(tu, report);
    }

    /// The id the database names `file` by, handed out on first use.
    static std::uint32_t path_id(index::ProjectIndex& index, Fid file) {
        return index.path_id(file);
    }

    /// The key of `file`'s records, its shards' and its manifest's.
    static std::string file_key(const index::ProjectIndex& index, Fid file) {
        return index.file_key(file);
    }

    /// Whether the file records the manifests derive (contributions, absent
    /// lookups, importers) hold no entry of any unit.
    static bool derived_tables_empty(const index::ProjectIndex& index) {
        return ranges::all_of(index.dependent_records.changed, [](const auto& entry) {
            return !entry.second || entry.second->empty();
        });
    }
};

}  // namespace clice::testing
