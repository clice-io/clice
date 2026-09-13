#include <array>
#include <bit>
#include <chrono>
#include <ctime>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <thread>

#include "driver/driver.h"
#include "index/database.h"
#include "index/serialization.h"
#include "sched/batch.h"
#include "sched/configuration.h"
#include "sched/context.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "server/service/query.h"
#include "support/cache_store.h"
#include "support/timer.h"

#include "kota/meta/enum.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct IndexOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to activate, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Number of indexing workers (default: from config)",
           required = false)
    <std::uint32_t> workers;

    DecoFlag(names = {"--stats"},
             help = "Print statistics of the persisted index instead of indexing",
             required = false)
    stats;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "How many of the largest file shards --stats lists",
           required = false)
    <std::uint32_t> top;

    DecoFlag(names = {"--variants"},
             help =
                 "With --stats: list every file shard with its variant count, one "
                 "tab-separated line each",
             required = false)
    variants;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--show-symbol", "--show-symbol="},
           help =
               "Print what the persisted index records about a symbol, named by its "
               "name, qualified name or #hash, instead of indexing",
           required = false)
    <std::string> show_symbol;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--show-file", "--show-file="},
           help =
               "Print a file's persisted rows: variants, contributing units and row "
               "counts, instead of indexing",
           required = false)
    <std::string> show_file;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--show-tu", "--show-tu="},
           help =
               "Print a translation unit's persisted manifest: include tree and "
               "contributions, instead of indexing",
           required = false)
    <std::string> show_tu;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level;
};

auto make_command() {
    return kota::deco::cli::command<IndexOptions>("clice index [OPTIONS]");
}

std::string format_size(std::uint64_t bytes) {
    if(bytes >= 1024 * 1024) {
        return std::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
    }
    if(bytes >= 1024) {
        return std::format("{:.1f} KB", bytes / 1024.0);
    }
    return std::format("{} B", bytes);
}

std::string format_hash(std::uint64_t hash) {
    return std::format("#{:016x}", hash);
}

/// Milliseconds since the epoch as a local wall-clock stamp.
std::string format_time(std::uint64_t epoch_ms) {
    auto seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    return stamp;
}

int run_indexing(std::string root,
                 std::string configuration,
                 std::uint32_t workers,
                 const char* self_path) {
    // Progress goes to stderr whatever the log level: a run spends most of
    // its time with nothing else to say, and the per-unit log lines exist
    // only at info level. The batch paces the reports; only a repeat of the
    // last snapshot (a round end followed by the tick) is dropped.
    auto started = std::chrono::steady_clock::now();
    std::optional<BatchProgress> last_printed;
    auto report_progress = [&](const BatchProgress& progress) {
        if(last_printed == progress) {
            return;
        }
        last_printed = progress;
        std::println(
            stderr,
            "progress {}/{} units, {} failed, {:.0f}s elapsed",
            progress.completed,
            progress.total,
            progress.failed,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    };

    auto result = run_batch_index({
        .root = std::move(root),
        .configuration = std::move(configuration),
        .workers = workers,
        .self_path = self_path,
        .on_progress = report_progress,
    });
    if(result.interrupted) {
        std::println("Indexing interrupted; progress saved. Rerun `clice index` to resume.");
        return result.exit_code;
    }
    if(!result.completed) {
        if(!result.log_dir.empty()) {
            std::println("Session log: {}", result.log_dir);
        }
        return result.exit_code;
    }
    std::println("Indexed {} translation unit{} in {:.1f}s: {} file shard{} ({}), {} symbol{}.",
                 result.indexed_tus,
                 plural_s(result.indexed_tus),
                 result.seconds,
                 result.shard_count,
                 plural_s(result.shard_count),
                 format_size(result.shard_bytes),
                 result.symbol_count,
                 plural_s(result.symbol_count));
    if(result.standalone_headers != 0) {
        std::println("{} header{} indexed standalone under a borrowed compile command.",
                     result.standalone_headers,
                     plural_s(result.standalone_headers));
    }
    if(!result.failed.empty()) {
        std::println("{} translation unit{} failed to index (see the log); the index is partial:",
                     result.failed.size(),
                     plural_s(result.failed.size()));
        constexpr std::size_t listed = 50;
        for(auto& path: result.failed | std::views::take(listed)) {
            std::println("  {}", path);
        }
        if(result.failed.size() > listed) {
            std::println("  ... and {} more", result.failed.size() - listed);
        }
    }
    if(result.unsaved) {
        std::println("Part of the index could not be persisted (see the log).");
    }
    if(!result.log_dir.empty()) {
        std::println("Session log: {}", result.log_dir);
    }
    return result.exit_code;
}

/// The persisted index of a workspace, opened read-only for one report.
struct IndexView {
    kota::event_loop loop;
    Workspace workspace;
    ContextResolver contexts{workspace};
    IndexStore store{loop, workspace, contexts};
    std::string configuration;

    /// Translation units the load dropped as stale or partially written.
    std::size_t pending = 0;

    const index::ProjectIndex& project() const {
        return workspace.project_index;
    }

    llvm::StringRef path_of(Fid file) const {
        return workspace.file_table.resolve(file);
    }
};

/// Sentinel of open_index: the load raced a live writer's batch; the caller
/// retries instead of reporting over the mid-write state.
constexpr int open_retry = -1;

/// Open the persisted index read-only into `view`. A non-zero return is
/// the command's exit code, the cause already logged; open_retry asks for
/// another attempt.
int open_index(IndexView& view,
               llvm::StringRef root,
               llvm::StringRef requested_configuration,
               bool allow_retry) {
    auto config = Config::load_from_workspace(root);
    if(!check_requested_configuration(config, requested_configuration)) {
        return 1;
    }
    auto configuration = resolve_configuration(config, requested_configuration);
    // Read-only: the default cache directory exists as soon as the config
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a stats reader.
    auto store =
        CacheStore::open(config.project.cache_dir, cache_format_version, /*read_only=*/true);
    if(!store) {
        if(store.error() == std::errc::no_such_file_or_directory) {
            LOG_ERROR("No index cache at {}; run `clice index` first",
                      std::string_view(config.project.cache_dir));
        } else {
            LOG_ERROR("Failed to open cache store at {}: {}",
                      std::string_view(config.project.cache_dir),
                      store.error().message());
        }
        return 1;
    }

    auto& workspace = view.workspace;
    workspace.config = std::move(config);
    workspace.store.emplace(std::move(*store));
    workspace.build.reset_active(configuration);
    workspace.index_db = index::open_database(*workspace.store, configuration);
    if(!workspace.index_db) {
        LOG_ERROR("No index cache at {}; run `clice index` first",
                  index::library_directory(*workspace.store, configuration));
        return 1;
    }
    view.configuration = configuration;
    auto loaded = view.store.load(/*read_only=*/true);
    if(!loaded.decoded) {
        LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                  std::string_view(workspace.config.project.cache_dir));
        return 1;
    }
    // load() detaches the storage when the global blob exists but cannot
    // be read — a transient IO error, not an empty index.
    if(workspace.index_db == nullptr) {
        LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
                  std::string_view(workspace.config.project.cache_dir));
        return 1;
    }
    // A live writer's save publishes shards and manifests before the
    // replacement global blob, so a read racing the batch can capture the
    // old global next to newer blobs; the load drops those as stale and
    // the verdicts below misread the mid-write state as damage. The writer
    // may already have finished and unlocked by the time any post-load
    // probe runs, so retry on the drops themselves; genuine damage merely
    // spends the bounded retries before the final no-retry pass reports it.
    view.pending = loaded.report.reindex().size();
    if(allow_retry && view.pending != 0) {
        return open_retry;
    }
    // With no pump attached the load report's debt can only be the
    // recovery drops: every TU's blobs were missing, stale, or corrupt — a
    // damaged cache, not a legitimately empty one.
    if(view.project().manifests.empty() && workspace.shards.empty() && view.pending != 0) {
        LOG_ERROR(
            "Index cache at {} has no servable data ({} translation units need "
            "reindexing); run `clice index` to rebuild",
            std::string_view(workspace.config.project.cache_dir),
            view.pending);
        return 1;
    }
    return 0;
}

/// Run `report` over the workspace's persisted index, opened read-only
/// with the mid-save retry.
int with_index(llvm::StringRef root,
               llvm::StringRef configuration,
               llvm::function_ref<int(IndexView&)> report) {
    constexpr std::uint32_t attempts = 5;
    for(std::uint32_t attempt = 1; attempt <= attempts; attempt += 1) {
        IndexView view;
        int rc = open_index(view, root, configuration, /*allow_retry=*/attempt < attempts);
        if(rc == open_retry) {
            LOG_DEBUG("Index cache is mid-save; retrying the read");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if(rc != 0) {
            return rc;
        }
        return report(view);
    }
    std::unreachable();
}

/// Counts bucketed by powers of two: 0, 1, 2-3, 4-7, ... — the shape of
/// the long-tailed distributions the index has (references per symbol,
/// variants per file).
struct Histogram {
    constexpr static std::size_t buckets = 12;

    std::array<std::uint64_t, buckets> counts{};
    std::uint64_t total = 0;

    void add(std::uint64_t value) {
        counts[std::min<std::size_t>(std::bit_width(value), buckets - 1)] += 1;
        total += 1;
    }

    static std::string label(std::size_t bucket) {
        if(bucket <= 1) {
            return std::to_string(bucket);
        }
        auto low = std::uint64_t(1) << (bucket - 1);
        if(bucket == buckets - 1) {
            return std::format("{}+", low);
        }
        return std::format("{}-{}", low, low * 2 - 1);
    }

    void print() const {
        for(std::size_t bucket = 0; bucket < buckets; bucket += 1) {
            if(counts[bucket] == 0) {
                continue;
            }
            std::println("  {:>8}  {:>9}  {:>5.1f}%",
                         label(bucket),
                         counts[bucket],
                         total != 0 ? 100.0 * static_cast<double>(counts[bucket]) /
                                          static_cast<double>(total)
                                    : 0.0);
        }
    }
};

struct ShardStat {
    llvm::StringRef path;
    std::uint64_t bytes = 0;
    std::size_t variants = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t relations = 0;
};

/// The byte split of the shard blobs, mirroring the ShardBlob columns.
struct ShardColumns {
    std::uint64_t content = 0;
    std::uint64_t variants = 0;
    std::uint64_t symbols = 0;
    std::uint64_t locals = 0;
    std::uint64_t occ_rows = 0;
    std::uint64_t occ_masks = 0;
    std::uint64_t rel_rows = 0;
    std::uint64_t rel_masks = 0;

    std::uint64_t total() const {
        return content + variants + symbols + locals + occ_rows + occ_masks + rel_rows + rel_masks;
    }
};

ShardColumns shard_columns_of(llvm::StringRef bytes) {
    ShardColumns columns;
    index::ShardBlob blob;
    if(!index::deserialize_blob(bytes, blob)) {
        return columns;
    }
    auto row_bytes = [](const index::RowRanges& rr) -> std::uint64_t {
        return rr.packed.size() * 4 + rr.begins.size() * 4 + rr.lengths.size() +
               rr.long_rows.size() * 4 + rr.long_ends.size() * 4;
    };
    auto mask_bytes = [](const index::RowRanges& rr) -> std::uint64_t {
        return rr.masks32.size() * 4 + rr.masks64.size() * 8 + rr.roaring_offsets.size() * 4 +
               rr.roaring.size();
    };
    columns.content = blob.content.size() + blob.line_lengths.size() +
                      blob.long_line_rows.size() * 4 + blob.long_line_lengths.size() * 4;
    columns.variants = blob.variants.size() * 8;
    columns.symbols = blob.sym_hashes.size() * 8 + blob.sym_rel_offsets.size() * 4;
    columns.locals = blob.local_syms.size() * 4 + blob.local_kinds.size() +
                     blob.local_scopes.size() + blob.local_parents.size() * 8 +
                     blob.local_flags.size() * 2;
    for(auto& name: blob.local_names) {
        columns.locals += name.size();
    }
    for(auto& args: blob.local_args) {
        columns.locals += args.size();
    }
    columns.occ_rows = row_bytes(blob.occs) + blob.occ_syms8.size() + blob.occ_syms16.size() * 2 +
                       blob.occ_syms32.size() * 4;
    columns.occ_masks = mask_bytes(blob.occs);
    columns.rel_rows = row_bytes(blob.rels) + blob.rel_kinds.size() + blob.rel_sym_rows.size() * 4 +
                       blob.rel_sym8.size() + blob.rel_sym16.size() * 2 +
                       blob.rel_sym32.size() * 4 + blob.rel_def_rows.size() * 4 +
                       blob.rel_def_begins.size() * 4 + blob.rel_def_ends.size() * 4;
    columns.rel_masks = mask_bytes(blob.rels);
    return columns;
}

/// The byte split of the global blob's symbol table; everything else in
/// the blob (file versions, the path table, framing) is the remainder of
/// its serialized size.
struct GlobalColumns {
    std::uint64_t names = 0;
    std::uint64_t args = 0;
    std::uint64_t bitmaps = 0;
    /// Hash, parent, kind, flags and file per symbol.
    std::uint64_t fixed = 0;
};

struct IndexStats {
    std::vector<ShardStat> shards;
    ShardColumns columns;
    std::uint64_t shard_bytes = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t relations = 0;
    std::uint64_t global_bytes = 0;
    GlobalColumns global;
    Histogram references_per_symbol;
    Histogram name_lengths;
    Histogram variants_per_shard;
};

IndexStats collect_stats(IndexView& view) {
    IndexStats stats;
    auto& workspace = view.workspace;
    auto& project = workspace.project_index;

    stats.shards.reserve(workspace.shards.size());
    for(auto& [path_id, shard]: workspace.shards) {
        ShardStat stat{.path = view.path_of(path_id),
                       .bytes = shard.bytes().size(),
                       .variants = shard.variants().size()};
        shard.for_each_occurrence([&](const index::Occurrence&) {
            stat.occurrences += 1;
            return true;
        });
        shard.for_each_relation([&](index::SymbolHash, const index::Relation&) {
            stat.relations += 1;
            return true;
        });
        stats.shard_bytes += stat.bytes;
        stats.occurrences += stat.occurrences;
        stats.relations += stat.relations;
        stats.variants_per_shard.add(stat.variants);
        auto columns = shard_columns_of(shard.bytes());
        stats.columns.content += columns.content;
        stats.columns.variants += columns.variants;
        stats.columns.symbols += columns.symbols;
        stats.columns.locals += columns.locals;
        stats.columns.occ_rows += columns.occ_rows;
        stats.columns.occ_masks += columns.occ_masks;
        stats.columns.rel_rows += columns.rel_rows;
        stats.columns.rel_masks += columns.rel_masks;
        stats.shards.push_back(stat);
    }
    std::ranges::sort(stats.shards, std::ranges::greater{}, &ShardStat::bytes);

    for(auto& symbol: llvm::make_second_range(project.symbols)) {
        stats.global.names += symbol.name.size();
        stats.global.args += symbol.args.size();
        stats.global.bitmaps += symbol.reference_files.getSizeInBytes(true);
        stats.global.fixed += 8 + 8 + 1 + 2 + 4;
        stats.references_per_symbol.add(symbol.reference_files.cardinality());
        stats.name_lengths.add(symbol.name.size());
    }
    std::string global;
    llvm::raw_string_ostream os(global);
    project.serialize_global(os, workspace.file_table);
    stats.global_bytes = global.size();
    return stats;
}

void print_stats(const IndexView& view, const IndexStats& stats, std::uint32_t top) {
    auto& workspace = view.workspace;
    auto& project = view.project();
    auto share = [](std::uint64_t bytes, std::uint64_t whole) {
        return whole != 0 ? 100.0 * static_cast<double>(bytes) / static_cast<double>(whole) : 0.0;
    };
    auto column = [&](llvm::StringRef name, std::uint64_t bytes, std::uint64_t whole) {
        std::println("  {:<24} {:>10}  {:>5.1f}%", name, format_size(bytes), share(bytes, whole));
    };

    std::println("Index cache: {}", index::library_directory(*workspace.store, view.configuration));
    if(!view.configuration.empty()) {
        std::println("Configuration: {}", view.configuration);
    }
    std::println("Translation units: {}", project.manifests.size());
    std::println("File shards: {} ({}), {} occurrences, {} relations",
                 stats.shards.size(),
                 format_size(stats.shard_bytes),
                 stats.occurrences,
                 stats.relations);
    std::println("Global symbols: {}, file versions: {}",
                 project.symbols.size(),
                 workspace.file_table.versions.size());
    if(view.pending != 0) {
        std::println(
            "Translation units pending reindex (stale or partially written): {}; "
            "run `clice index` to repair",
            view.pending);
    }

    auto payload = stats.columns.total();
    std::println();
    std::println("Shard payload by column ({}; the rest of the file size is format framing):",
                 format_size(payload));
    column("occurrence rows", stats.columns.occ_rows, payload);
    column("occurrence masks", stats.columns.occ_masks, payload);
    column("relation rows", stats.columns.rel_rows, payload);
    column("relation masks", stats.columns.rel_masks, payload);
    column("symbol tables", stats.columns.symbols, payload);
    column("local symbols", stats.columns.locals, payload);
    column("content + line maps", stats.columns.content, payload);
    column("variant tables", stats.columns.variants, payload);

    auto& global = stats.global;
    auto symbol_bytes = global.names + global.args + global.bitmaps + global.fixed;
    std::println();
    std::println("Global blob ({}):", format_size(stats.global_bytes));
    column("symbol names", global.names, stats.global_bytes);
    column("specialization args", global.args, stats.global_bytes);
    column("reference bitmaps", global.bitmaps, stats.global_bytes);
    column("symbol fixed columns", global.fixed, stats.global_bytes);
    column("file versions + paths",
           stats.global_bytes > symbol_bytes ? stats.global_bytes - symbol_bytes : 0,
           stats.global_bytes);

    std::println();
    std::println("Symbols by reference file count:");
    stats.references_per_symbol.print();
    std::println();
    std::println("Symbols by name length:");
    stats.name_lengths.print();
    std::println();
    std::println("File shards by variant count:");
    stats.variants_per_shard.print();

    std::println();
    std::println("Top {} file shards by size:", std::min<std::size_t>(top, stats.shards.size()));
    for(auto& stat: stats.shards | std::views::take(top)) {
        std::println("  {:>10}  {:>4} variants  {:>9} occs  {:>9} rels  {}",
                     format_size(stat.bytes),
                     stat.variants,
                     stat.occurrences,
                     stat.relations,
                     std::string_view(stat.path));
    }
}

/// Every file shard with its variant count, most variants first — the
/// machine-readable form for cross-run comparisons.
void print_variants(const IndexStats& stats) {
    auto by_variants = stats.shards;
    std::ranges::stable_sort(by_variants, std::ranges::greater{}, &ShardStat::variants);
    std::println();
    std::println("variants\tpath");
    for(auto& stat: by_variants) {
        std::println("{}\t{}", stat.variants, std::string_view(stat.path));
    }
}

int run_stats(IndexView& view, std::uint32_t top, bool variants) {
    if(view.project().manifests.empty() && view.workspace.shards.empty()) {
        std::println("Index is empty; run `clice index` to build it.");
        return 0;
    }
    auto stats = collect_stats(view);
    print_stats(view, stats, top);
    if(variants) {
        print_variants(stats);
    }
    // Partial damage is still damage: automation must not read exit 0 as
    // "the cache is healthy" just because some TUs remained servable.
    return view.pending == 0 ? 0 : 1;
}

std::string flag_names(index::SymbolFlags flags) {
    llvm::SmallVector<llvm::StringRef> names;
    constexpr std::pair<index::SymbolFlags, llvm::StringRef> bits[] = {
        {index::SymbolFlags::HasDefinition,   "HasDefinition"  },
        {index::SymbolFlags::Template,        "Template"       },
        {index::SymbolFlags::Specialization,  "Specialization" },
        {index::SymbolFlags::Deprecated,      "Deprecated"     },
        {index::SymbolFlags::InlineNamespace, "InlineNamespace"},
        {index::SymbolFlags::Unnamed,         "Unnamed"        },
        {index::SymbolFlags::SpelledInMacro,  "SpelledInMacro" },
        {index::SymbolFlags::SystemHeader,    "SystemHeader"   },
        {index::SymbolFlags::Completable,     "Completable"    },
    };
    for(auto [bit, name]: bits) {
        if(index::has_flag(flags, bit)) {
            names.push_back(name);
        }
    }
    return llvm::join(names, ",");
}

llvm::StringRef kind_name(SymbolKind kind) {
    return kota::meta::enum_name(static_cast<SymbolKind::Kind>(kind), "Invalid");
}

/// The symbol a `--show-symbol` argument names: `#<hex>` is a hash, anything
/// else a display name (`Box<int>`) or a qualified one (`ns::Box<int>`).
std::vector<index::SymbolHash> matching_symbols(IndexView& view,
                                                IndexQuery& query,
                                                llvm::StringRef wanted) {
    std::vector<index::SymbolHash> matches;
    if(wanted.consume_front("#")) {
        index::SymbolHash hash = 0;
        if(!wanted.getAsInteger(16, hash) && !index::reserved_key(hash)) {
            matches.push_back(hash);
        }
        return matches;
    }
    bool qualified = wanted.contains("::");
    for(auto& [hash, symbol]: view.project().symbols) {
        if(symbol.name + symbol.args == wanted ||
           (qualified && query.qualified_name(hash) == wanted)) {
            matches.push_back(hash);
        }
    }
    std::ranges::sort(matches);
    return matches;
}

int run_show_symbol(IndexView& view, llvm::StringRef wanted) {
    IndexQuery query(view.workspace, {});
    auto matches = matching_symbols(view, query, wanted);
    if(matches.empty()) {
        std::println(
            "No symbol named {} in the index (names cover the global table; "
            "file-local symbols are reachable by #hash).",
            std::string_view(wanted));
        return 1;
    }
    int rc = 0;
    for(auto hash: matches) {
        auto info = query.symbol_info(hash);
        if(!info) {
            std::println("{}: no table knows this hash", format_hash(hash));
            rc = 1;
            continue;
        }
        std::println("symbol {}  kind={}  name={}  args={}  qualified={}  flags={}  form={}",
                     format_hash(hash),
                     kind_name(info->kind),
                     info->name,
                     info->args,
                     query.qualified_name(hash),
                     flag_names(info->flags),
                     kota::meta::enum_name(index::name_form(info->flags), "Other"));
        // A persisted parent column can be cyclic; the tables only reject
        // reserved values.
        llvm::DenseSet<index::SymbolHash> visited{hash};
        for(auto parent = info->parent; parent != 0 && visited.insert(parent).second;) {
            auto scope = query.symbol_info(parent);
            if(!scope) {
                std::println("  parent {}: unknown", format_hash(parent));
                break;
            }
            std::println("  parent {}  kind={}  name={}",
                         format_hash(parent),
                         kind_name(scope->kind),
                         scope->display_name());
            parent = scope->parent;
        }
        auto it = view.project().symbols.find(hash);
        if(it != view.project().symbols.end()) {
            auto& symbol = it->second;
            std::println("  scope={}  file={}  reference files={}",
                         kota::meta::enum_name(symbol.scope, "External"),
                         symbol.file == index::no_file ? "-" : view.path_of(Fid{symbol.file}),
                         symbol.reference_files.cardinality());
        } else {
            std::println("  scope=local (not in the global table)");
        }

        struct Counts {
            std::size_t definitions = 0;
            std::size_t declarations = 0;
            std::size_t references = 0;
        };

        // Straight from every shard: the query's fan-out follows the global
        // table's reference bitmaps, which a file-local symbol has no entry
        // in.
        std::map<std::string, Counts> per_file;
        for(auto& [path_id, shard]: view.workspace.shards) {
            auto count = [&](RelationKind kind, std::size_t Counts::* field) {
                shard.lookup(hash, kind, [&](const index::Relation&) {
                    per_file[view.path_of(path_id).str()].*field += 1;
                    return true;
                });
            };
            count(RelationKind::Definition, &Counts::definitions);
            count(RelationKind::Declaration, &Counts::declarations);
            count(RelationKind::Reference, &Counts::references);
        }
        for(auto& [path, counts]: per_file) {
            std::println("  {}: definitions={} declarations={} references={}",
                         path,
                         counts.definitions,
                         counts.declarations,
                         counts.references);
        }
    }
    return rc;
}

int run_show_file(IndexView& view, llvm::StringRef argument) {
    auto path = workspace_root(argument);
    auto file = view.workspace.file_table.find(path);
    auto shard_it = file ? view.workspace.shards.find(*file) : view.workspace.shards.end();
    if(shard_it == view.workspace.shards.end()) {
        std::println("No rows for {} in the index.", path);
        return 1;
    }
    auto& shard = shard_it->second;
    std::println("file {}", path);
    std::println("  blob={}  content size={}  content hash={}  text={}",
                 format_size(shard.bytes().size()),
                 shard.content_size(),
                 format_hash(shard.content_hash()),
                 shard.content().empty() ? "not stored (ASCII)" : "stored");

    // Which unit contributed which variant, from the manifests.
    std::map<std::uint64_t, std::vector<llvm::StringRef>> contributors;
    if(auto it = view.project().contributions.find(*file);
       it != view.project().contributions.end()) {
        for(auto& [tu, hash]: it->second) {
            contributors[hash].push_back(view.path_of(tu));
        }
    }
    auto variants = shard.variants();
    std::println("  variants={}", variants.size());
    for(auto hash: variants) {
        auto& units = contributors[hash];
        std::ranges::sort(units);
        std::println("    {}  contributed by {} unit{}",
                     format_hash(hash),
                     units.size(),
                     plural_s(units.size()));
        for(auto unit: units) {
            std::println("      {}", unit);
        }
    }

    std::size_t occurrences = 0;
    shard.for_each_occurrence([&](const index::Occurrence&) {
        occurrences += 1;
        return true;
    });
    std::map<llvm::StringRef, std::size_t> by_kind;
    std::size_t relations = 0;
    shard.for_each_relation([&](index::SymbolHash, const index::Relation& relation) {
        by_kind[kota::meta::enum_name(relation.kind, "Invalid")] += 1;
        relations += 1;
        return true;
    });
    index::ShardBlob blob;
    index::deserialize_blob(shard.bytes(), blob);
    std::println("  symbols={}  local symbols={}  occurrences={}  relations={}",
                 blob.sym_hashes.size(),
                 blob.local_syms.size(),
                 occurrences,
                 relations);
    for(auto& [kind, count]: by_kind) {
        std::println("    {}={}", kind, count);
    }
    for(std::size_t k = 0; k < blob.local_syms.size(); k += 1) {
        std::println("    local {}  kind={}  name={}{}",
                     format_hash(blob.sym_hashes[blob.local_syms[k]]),
                     kind_name(SymbolKind(blob.local_kinds[k])),
                     blob.local_names[k],
                     blob.local_args[k]);
    }
    return 0;
}

int run_show_tu(IndexView& view, llvm::StringRef argument) {
    auto path = workspace_root(argument);
    auto& files = view.workspace.file_table;
    auto tu = files.find(path);
    auto manifest_it = tu ? view.project().manifests.find(*tu) : view.project().manifests.end();
    if(manifest_it == view.project().manifests.end()) {
        std::println("No manifest for {} in the index.", path);
        return 1;
    }
    auto& manifest = manifest_it->second;
    auto version_path = [&](VersionID fv) -> llvm::StringRef {
        return files.knows_version(fv) ? files.resolve(files.version(fv).fid) : "<unknown>";
    };
    std::println("translation unit {}", path);
    std::println("  built at {}  generation={}  content hash={}",
                 format_time(manifest.built_at),
                 manifest.global_gen,
                 format_hash(files.knows_version(manifest.tu_fv)
                                 ? files.version(manifest.tu_fv).content_hash
                                 : 0));

    std::println("  contributions={}", manifest.contributions.size());
    for(auto& [fv, hash]: manifest.contributions) {
        std::println("    {}  {}", format_hash(hash), version_path(fv));
    }

    // The include tree, children under their parent in node order; a
    // node's line is the directive's line in the file that includes it.
    std::println("  include tree ({} node{}):",
                 manifest.nodes.size(),
                 plural_s(manifest.nodes.size()));
    std::vector<std::vector<std::uint32_t>> children(manifest.nodes.size() + 1);
    for(std::uint32_t i = 0; i < manifest.nodes.size(); i += 1) {
        auto parent = manifest.nodes[i].parent;
        children[parent == index::no_node ? manifest.nodes.size() : parent].push_back(i);
    }
    auto print = [&](auto& self, std::uint32_t node, std::size_t depth) -> void {
        auto& entry = manifest.nodes[node];
        auto includer = entry.parent == index::no_node
                            ? llvm::StringRef(path)
                            : version_path(VersionID{manifest.nodes[entry.parent].file});
        std::println("    {:{}}{}  included at {}:{}",
                     "",
                     depth * 2,
                     version_path(VersionID{entry.file}),
                     includer,
                     entry.line);
        for(auto child: children[node]) {
            self(self, child, depth + 1);
        }
    };
    for(auto root: children.back()) {
        print(print, root, 0);
    }
    return 0;
}

}  // namespace

void add_index(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](IndexOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           logging::stderr_logger("index", logging::options);

           auto ws = workspace_root(opts.workspace.value_or(""));
           auto configuration = opts.configuration.value_or("");
           if(opts.show_symbol) {
               exit_code = with_index(ws, configuration, [&](IndexView& view) {
                   return run_show_symbol(view, *opts.show_symbol);
               });
               return;
           }
           if(opts.show_file) {
               exit_code = with_index(ws, configuration, [&](IndexView& view) {
                   return run_show_file(view, *opts.show_file);
               });
               return;
           }
           if(opts.show_tu) {
               exit_code = with_index(ws, configuration, [&](IndexView& view) {
                   return run_show_tu(view, *opts.show_tu);
               });
               return;
           }
           if(opts.stats) {
               exit_code = with_index(ws, configuration, [&](IndexView& view) {
                   return run_stats(view, opts.top.value_or(20), static_cast<bool>(opts.variants));
               });
               return;
           }
           exit_code = run_indexing(std::move(ws),
                                    std::move(configuration),
                                    opts.workers.value_or(0),
                                    self_path);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "index", .description = "Index a workspace ahead of time"}, std::move(cmd));
}

}  // namespace clice::driver
