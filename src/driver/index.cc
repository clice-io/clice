#include <array>
#include <bit>
#include <chrono>
#include <ctime>
#include <format>
#include <map>
#include <print>
#include <ranges>

#include "driver/driver.h"
#include "index/database.h"
#include "index/query.h"
#include "index/serialization.h"
#include "index/writer_lock.h"
#include "sched/batch.h"
#include "sched/configuration.h"
#include "sched/open_index.h"
#include "sched/workspace.h"
#include "server/transport/control_client.h"
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
                 "Print the statistics and then every file shard with its variant "
                 "count, one tab-separated line each",
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

/// Index through the serving writer: the editor's server holds the cache
/// directory's writer lock, so it runs the sweep and the command waits
/// for the rows to land.
int run_indexing_via_server(const index::ServerEndpoint& endpoint, llvm::StringRef configuration) {
    auto result = control::request_index(endpoint, configuration);
    if(!result) {
        LOG_ERROR("{}", result.error());
        return 1;
    }
    std::println("Indexed through the running clice server (pid {}).", endpoint.pid);
    if(!result->failed.empty()) {
        std::println(
            "{} translation unit{} failed to index (see the server log); the index is partial:",
            result->failed.size(),
            plural_s(result->failed.size()));
        for(auto& path: result->failed) {
            std::println("  {}", path);
        }
        return 1;
    }
    return 0;
}

int run_indexing(std::string root,
                 std::string configuration,
                 std::uint32_t workers,
                 const char* self_path) {
    auto config = Config::load_from_workspace(root);
    if(!check_requested_configuration(config, configuration)) {
        return 1;
    }
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::probe_writer(cache_dir);
    switch(writer.state) {
        case index::WriterProbe::State::Free: break;
        case index::WriterProbe::State::Server:
            return run_indexing_via_server(writer.endpoint,
                                           resolve_configuration(config, configuration));
        case index::WriterProbe::State::Held: {
            LOG_ERROR("{}", index::held_writer_message(writer, cache_dir));
            return 1;
        }
    }
    // Progress goes to stderr whatever the log level: a run spends most of
    // its time with nothing else to say, and the per-unit log lines exist
    // only at info level. The batch paces the reports, and a tick with the
    // counts unchanged is the heartbeat of a unit that takes longer than
    // the pace.
    auto started = std::chrono::steady_clock::now();
    auto report_progress = [&](const BatchProgress& progress) {
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
        std::println(
            "The index holds {} header{} indexed standalone under borrowed compile "
            "commands.",
            result.standalone_headers,
            plural_s(result.standalone_headers));
    }
    if(!result.failed.empty()) {
        std::println("{} translation unit{} failed to index (see the log); the index is partial:",
                     result.failed.size(),
                     plural_s(result.failed.size()));
        for(auto& path: result.failed) {
            std::println("  {}", path);
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
    std::uint64_t search_bytes = 0;
    GlobalColumns global;
    Histogram references_per_symbol;
    Histogram name_lengths;
    Histogram variants_per_shard;
};

IndexStats collect_stats(Workspace& workspace) {
    IndexStats stats;
    auto& project = workspace.project_index;

    stats.shards.reserve(workspace.project_index.shards.size());
    for(auto& [path_id, shard]: workspace.project_index.shards) {
        ShardStat stat{.path = workspace.file_table.resolve(path_id),
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

    auto columns = project.global_columns();
    stats.global.names = columns.names;
    stats.global.args = columns.args;
    stats.global.bitmaps = columns.bitmaps;
    stats.global.fixed = columns.fixed;
    project.for_each_symbol(
        [&](index::SymbolHash, const index::SymbolIdentity& symbol, std::uint32_t references) {
            stats.references_per_symbol.add(references);
            stats.name_lengths.add(symbol.name.size());
            return true;
        });
    if(auto blob = workspace.index_db->read(index::IndexBlobKind::Global, "global")) {
        stats.global_bytes = blob.buffer->getBufferSize();
    }
    if(auto blob = workspace.index_db->read(index::IndexBlobKind::Search, "search")) {
        stats.search_bytes = blob.buffer->getBufferSize();
    }
    return stats;
}

void print_stats(const Workspace& workspace,
                 llvm::ArrayRef<Fid> dropped,
                 const IndexStats& stats,
                 std::uint32_t top) {
    auto& project = workspace.project_index;
    auto share = [](std::uint64_t bytes, std::uint64_t whole) {
        return whole != 0 ? 100.0 * static_cast<double>(bytes) / static_cast<double>(whole) : 0.0;
    };
    auto column = [&](llvm::StringRef name, std::uint64_t bytes, std::uint64_t whole) {
        std::println("  {:<24} {:>10}  {:>5.1f}%", name, format_size(bytes), share(bytes, whole));
    };

    auto configuration = workspace.build.active_configuration();
    std::println("Index cache: {}", index::library_directory(*workspace.store, configuration));
    if(!configuration.empty()) {
        std::println("Configuration: {}", configuration);
    }
    std::println("Translation units: {}", project.manifests.size());
    std::println("File shards: {} ({}), {} occurrences, {} relations",
                 stats.shards.size(),
                 format_size(stats.shard_bytes),
                 stats.occurrences,
                 stats.relations);
    std::println("Global symbols: {}, file versions: {}",
                 project.symbol_count(),
                 workspace.file_table.versions.size());
    std::println("Search index: {} symbols ({}), {} merged since its build",
                 workspace.project_index.search_index.size(),
                 format_size(stats.search_bytes),
                 workspace.project_index.search_pending.size());
    if(!dropped.empty()) {
        std::println(
            "Translation units pending reindex (stale or partially written): {}; "
            "run `clice index` to repair",
            dropped.size());
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

int run_stats(Workspace& workspace, llvm::ArrayRef<Fid> dropped, std::uint32_t top, bool variants) {
    if(workspace.project_index.manifests.empty() && workspace.project_index.shards.empty()) {
        std::println("Index is empty; run `clice index` to build it.");
        return 0;
    }
    auto stats = collect_stats(workspace);
    print_stats(workspace, dropped, stats, top);
    if(variants) {
        print_variants(stats);
    }
    // Partial damage is still damage: automation must not read exit 0 as
    // "the cache is healthy" just because some TUs remained servable.
    return dropped.empty() ? 0 : 1;
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
std::vector<index::SymbolHash> matching_symbols(Workspace& workspace,
                                                index::IndexQuery& query,
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
    workspace.project_index.for_each_symbol(
        [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, std::uint32_t) {
            if((symbol.name + symbol.args).str() == wanted ||
               (qualified && query.qualified_name(hash) == wanted)) {
                matches.push_back(hash);
            }
            return true;
        });
    std::ranges::sort(matches);
    return matches;
}

int run_show_symbol(Workspace& workspace, llvm::StringRef wanted) {
    index::IndexQuery query(workspace.project_index, workspace.file_table, nullptr, nullptr);
    auto matches = matching_symbols(workspace, query, wanted);
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
        if(auto symbol = workspace.project_index.identity_of(hash)) {
            std::println("  scope={}  file={}  reference files={}",
                         kota::meta::enum_name(symbol->scope, "External"),
                         symbol->file == index::no_file
                             ? "-"
                             : workspace.file_table.resolve(Fid{symbol->file}),
                         workspace.project_index.reference_count(hash));
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
        for(auto& [path_id, shard]: workspace.project_index.shards) {
            auto count = [&](RelationKind kind, std::size_t Counts::* field) {
                shard.lookup(hash, kind, [&](const index::Relation&) {
                    per_file[workspace.file_table.resolve(path_id).str()].*field += 1;
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

int run_show_file(Workspace& workspace, llvm::StringRef argument) {
    auto path = inspected_path(workspace, argument);
    auto file = workspace.file_table.find(path);
    auto shard_it =
        file ? workspace.project_index.shards.find(*file) : workspace.project_index.shards.end();
    if(shard_it == workspace.project_index.shards.end()) {
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
    if(auto it = workspace.project_index.contributions.find(*file);
       it != workspace.project_index.contributions.end()) {
        for(auto& [tu, hash]: it->second) {
            contributors[hash].push_back(workspace.file_table.resolve(tu));
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

int run_show_tu(Workspace& workspace, llvm::StringRef argument) {
    auto path = inspected_path(workspace, argument);
    auto& files = workspace.file_table;
    auto tu = files.find(path);
    auto& project = workspace.project_index;
    auto manifest_it = tu ? project.manifests.find(*tu) : project.manifests.end();
    if(manifest_it == project.manifests.end()) {
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
    std::size_t printed = 0;
    auto print = [&](auto& self, std::uint32_t node, std::size_t depth) -> void {
        printed += 1;
        auto& entry = manifest.nodes[node];
        auto includer = entry.parent == index::no_node
                            ? llvm::StringRef(path)
                            : version_path(VersionID{manifest.nodes[entry.parent].file});
        std::println("    {:{}}{}  {} at {}:{}",
                     "",
                     depth * 2,
                     version_path(VersionID{entry.file}),
                     entry.skipped ? "skipped" : "included",
                     includer,
                     entry.line);
        for(auto child: children[node]) {
            self(self, child, depth + 1);
        }
    };
    for(auto root: children.back()) {
        print(print, root, 0);
    }
    // A manifest is accepted with in-range parents only; a cycle hangs off
    // no root and would otherwise vanish from the listing.
    if(printed != manifest.nodes.size()) {
        std::println("    {} node{} unreachable from the root (cyclic parents)",
                     manifest.nodes.size() - printed,
                     plural_s(manifest.nodes.size() - printed));
        return 1;
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
           std::size_t modes = (opts.show_symbol ? 1 : 0) + (opts.show_file ? 1 : 0) +
                               (opts.show_tu ? 1 : 0) + (opts.stats || opts.variants ? 1 : 0);
           if(modes > 1) {
               LOG_ERROR(
                   "--stats, --variants, --show-symbol, --show-file and --show-tu are "
                   "separate modes; pass one of them");
               return;
           }
           if(opts.show_symbol || opts.show_file || opts.show_tu || opts.stats || opts.variants) {
               Workspace workspace;
               ContextResolver contexts{workspace};
               auto loaded =
                   load_index(workspace, contexts, ws, configuration, /*with_build=*/false);
               if(!loaded) {
                   exit_code = 1;
               } else if(opts.show_symbol) {
                   exit_code = run_show_symbol(workspace, *opts.show_symbol);
               } else if(opts.show_file) {
                   exit_code = run_show_file(workspace, *opts.show_file);
               } else if(opts.show_tu) {
                   exit_code = run_show_tu(workspace, *opts.show_tu);
               } else {
                   exit_code = run_stats(workspace,
                                         loaded->dropped,
                                         opts.top.value_or(20),
                                         static_cast<bool>(opts.variants));
               }
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
