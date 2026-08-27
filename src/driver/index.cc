#include <chrono>
#include <format>
#include <print>
#include <ranges>
#include <thread>

#include "driver/driver.h"
#include "index/database.h"
#include "index/serialization.h"
#include "sched/batch.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/timer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"

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

int run_indexing(std::string root, std::uint32_t workers, const char* self_path) {
    auto result = run_batch_index({
        .root = std::move(root),
        .workers = workers,
        .self_path = self_path,
    });
    if(result.interrupted) {
        std::println("Indexing interrupted; progress saved. Rerun `clice index` to resume.");
        return result.exit_code;
    }
    if(!result.completed) {
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
    if(result.failed_files != 0) {
        std::println("{} translation unit{} failed to index (see the log); the index is partial.",
                     result.failed_files,
                     plural_s(result.failed_files));
    }
    if(result.unsaved) {
        std::println("Part of the index could not be persisted (see the log).");
    }
    return result.exit_code;
}

/// Sentinel of run_stats_once: the load raced a live writer's batch;
/// the caller retries instead of reporting over the mid-write state.
constexpr int stats_retry = -1;

int run_stats_once(llvm::StringRef root, std::uint32_t top, bool allow_retry) {
    auto config = Config::load_from_workspace(root);
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

    kota::event_loop loop;
    Workspace workspace;
    workspace.config = std::move(config);
    workspace.store.emplace(std::move(*store));
    workspace.index_db = index::open_database(*workspace.store, workspace.config.project.index_db);
    // A namespace whose directory scan failed looks empty while its blobs
    // exist — reporting "Index is empty" with exit code 0 would be a lie.
    if(auto ec = workspace.store->scan_error()) {
        LOG_ERROR("Failed to read the index cache at {}: {}",
                  std::string_view(workspace.config.project.cache_dir),
                  ec.message());
        return 1;
    }
    IndexStore index_store(loop, workspace);
    auto loaded = index_store.load(/*read_only=*/true);
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
    auto pending = loaded.report.reindex().size();
    if(allow_retry && pending != 0) {
        return stats_retry;
    }

    auto& project = workspace.project_index;
    if(project.manifests.empty() && workspace.shards.empty()) {
        // With no pump attached the load report's debt can only be the
        // recovery drops: every TU's blobs were missing, stale, or
        // corrupt — a damaged cache, not a legitimately empty one.
        if(pending != 0) {
            LOG_ERROR(
                "Index cache at {} has no servable data ({} translation units need "
                "reindexing); run `clice index` to rebuild",
                std::string_view(workspace.config.project.cache_dir),
                pending);
            return 1;
        }
        std::println("Index is empty; run `clice index` to build it.");
        return 0;
    }

    struct ShardStat {
        llvm::StringRef path;
        std::uint64_t bytes;
        std::size_t variants;
        std::uint64_t occurrences = 0;
        std::uint64_t relations = 0;
    };

    struct ColumnBytes {
        std::uint64_t content = 0;
        std::uint64_t variants = 0;
        std::uint64_t symbols = 0;
        std::uint64_t local_names = 0;
        std::uint64_t occ_rows = 0;
        std::uint64_t occ_masks = 0;
        std::uint64_t rel_rows = 0;
        std::uint64_t rel_masks = 0;
    } columns;

    auto row_bytes = [](const index::RowRanges& rr) -> std::uint64_t {
        return rr.packed.size() * 4 + rr.begins.size() * 4 + rr.lengths.size() +
               rr.long_rows.size() * 4 + rr.long_ends.size() * 4;
    };
    auto mask_bytes = [](const index::RowRanges& rr) -> std::uint64_t {
        return rr.masks32.size() * 4 + rr.masks64.size() * 8 + rr.roaring_offsets.size() * 4 +
               rr.roaring.size();
    };

    std::vector<ShardStat> files;
    files.reserve(workspace.shards.size());
    std::uint64_t total_bytes = 0, total_occurrences = 0, total_relations = 0;
    for(auto& [path_id, shard]: workspace.shards) {
        ShardStat stat{.path = workspace.path_pool.resolve(path_id),
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
        total_bytes += stat.bytes;
        total_occurrences += stat.occurrences;
        total_relations += stat.relations;
        files.push_back(stat);

        index::ShardBlob blob;
        if(index::deserialize_blob(shard.bytes(), blob)) {
            columns.content += blob.content.size() + blob.line_lengths.size() +
                               blob.long_line_rows.size() * 4 + blob.long_line_lengths.size() * 4;
            columns.variants += blob.variants.size() * 8;
            columns.symbols += blob.sym_hashes.size() * 8 + blob.sym_rel_offsets.size() * 4;
            columns.local_names +=
                blob.local_syms.size() * 4 + blob.local_kinds.size() + blob.local_scopes.size();
            for(auto& name: blob.local_names) {
                columns.local_names += name.size();
            }
            columns.occ_rows += row_bytes(blob.occs) + blob.occ_syms8.size() +
                                blob.occ_syms16.size() * 2 + blob.occ_syms32.size() * 4;
            columns.occ_masks += mask_bytes(blob.occs);
            columns.rel_rows += row_bytes(blob.rels) + blob.rel_kinds.size() +
                                blob.rel_sym_rows.size() * 4 + blob.rel_sym8.size() +
                                blob.rel_sym16.size() * 2 + blob.rel_sym32.size() * 4 +
                                blob.rel_def_rows.size() * 4 + blob.rel_def_begins.size() * 4 +
                                blob.rel_def_ends.size() * 4;
            columns.rel_masks += mask_bytes(blob.rels);
        }
    }
    std::ranges::sort(files, std::ranges::greater{}, &ShardStat::bytes);

    std::println("Index cache: {}", std::string_view(workspace.store->base_dir()));
    std::println("Translation units: {}", project.manifests.size());
    std::println("File shards: {} ({}), {} occurrences, {} relations",
                 files.size(),
                 format_size(total_bytes),
                 total_occurrences,
                 total_relations);
    std::println("Global symbols: {}, file versions: {}",
                 project.symbols.size(),
                 project.file_versions.size());

    auto payload = columns.content + columns.variants + columns.symbols + columns.local_names +
                   columns.occ_rows + columns.occ_masks + columns.rel_rows + columns.rel_masks;
    auto share = [&](std::uint64_t bytes) {
        return payload != 0 ? 100.0 * static_cast<double>(bytes) / static_cast<double>(payload)
                            : 0.0;
    };
    std::println();
    std::println("Payload by column ({}; the rest of the file size is format framing):",
                 format_size(payload));
    std::println("  occurrence rows      {:>10}  {:>5.1f}%",
                 format_size(columns.occ_rows),
                 share(columns.occ_rows));
    std::println("  occurrence masks     {:>10}  {:>5.1f}%",
                 format_size(columns.occ_masks),
                 share(columns.occ_masks));
    std::println("  relation rows        {:>10}  {:>5.1f}%",
                 format_size(columns.rel_rows),
                 share(columns.rel_rows));
    std::println("  relation masks       {:>10}  {:>5.1f}%",
                 format_size(columns.rel_masks),
                 share(columns.rel_masks));
    std::println("  symbol tables        {:>10}  {:>5.1f}%",
                 format_size(columns.symbols),
                 share(columns.symbols));
    std::println("  local symbol names   {:>10}  {:>5.1f}%",
                 format_size(columns.local_names),
                 share(columns.local_names));
    std::println("  content + line maps  {:>10}  {:>5.1f}%",
                 format_size(columns.content),
                 share(columns.content));
    std::println("  variant tables       {:>10}  {:>5.1f}%",
                 format_size(columns.variants),
                 share(columns.variants));
    if(pending != 0) {
        std::println(
            "Translation units pending reindex (stale or partially written): {}; "
            "run `clice index` to repair",
            pending);
    }
    std::println();
    std::println("Top {} file shards by size:", std::min<std::size_t>(top, files.size()));
    for(auto& stat: files | std::views::take(top)) {
        std::println("  {:>10}  {:>4} variants  {:>9} occs  {:>9} rels  {}",
                     format_size(stat.bytes),
                     stat.variants,
                     stat.occurrences,
                     stat.relations,
                     std::string_view(stat.path));
    }
    // Partial damage is still damage: automation must not read exit 0 as
    // "the cache is healthy" just because some TUs remained servable.
    return pending == 0 ? 0 : 1;
}

int run_stats(llvm::StringRef root, std::uint32_t top) {
    constexpr std::uint32_t stats_attempts = 5;
    for(std::uint32_t attempt = 1; attempt < stats_attempts; attempt += 1) {
        int rc = run_stats_once(root, top, /*allow_retry=*/true);
        if(rc != stats_retry) {
            return rc;
        }
        LOG_DEBUG("Index cache is mid-save; retrying the stats read");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return run_stats_once(root, top, /*allow_retry=*/false);
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

           llvm::SmallString<256> workspace(opts.workspace.value_or(""));
           if(workspace.empty()) {
               llvm::sys::fs::current_path(workspace);
           } else {
               llvm::sys::fs::make_absolute(workspace);
           }
           std::string ws(workspace.str());
           path::canonicalize(ws);

           if(opts.stats) {
               exit_code = run_stats(ws, opts.top.value_or(20));
               return;
           }
           exit_code = run_indexing(std::move(ws), opts.workers.value_or(0), self_path);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "index", .description = "Index a workspace ahead of time"}, std::move(cmd));
}

}  // namespace clice::driver
