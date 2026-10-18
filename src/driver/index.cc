module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :config.config;
import :driver.driver;
import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_table;
import :index.query;
import :index.search_index;
import :index.symbol_query;
import :index.tu_manifest;
import :index.writer_lock;
import :project.command_resolver;
import :project.configuration;
import :project.index_store;
import :project.open_index;
import :project.project;
import :sched.batch;
import :server.control_client;
import :server.editor_context;
import :server.query_commands;
import :support.text;
import :support.timer;

namespace clice::driver {

namespace {

struct IndexOptions {
    kota::deco::decl::HelpOption help;
    WorkspaceOptions where;
    JobsOption jobs;
    JsonReportOption report;
    LogLevelOption log;
};

struct StatsOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(help = "How many of the largest file shards to list (default 20)", required = false)
    <std::uint32_t> limit;

    DecoFlag(names = {"--variants"},
             help = "Then every file shard with its variant count, one tab-separated line each",
             required = false)
    variants;

    WorkspaceOptions where;
    LogLevelOption log;
};

struct SymbolOptions {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;
    WorkspaceOptions where;
    LogLevelOption log;
};

struct FileOptions {
    kota::deco::decl::HelpOption help;
    FileTarget target;
    WorkspaceOptions where;
    LogLevelOption log;
};

std::string format_hash(std::uint64_t hash) {
    return std::format("#{:016x}", hash);
}

/// Milliseconds since the epoch as a local wall-clock timestamp.
std::string format_time(std::uint64_t epoch_ms) {
    auto seconds = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    return timestamp;
}

/// Index through the serving writer: the editor's server holds the cache
/// directory's writer lock, so it runs the pass and the command waits
/// for the index to be committed.
int run_indexing_via_server(const index::ServerEndpoint& endpoint,
                            llvm::StringRef configuration,
                            bool json) {
    auto result = control::request_index(endpoint, configuration);
    if(!result) {
        LOG_ERROR("{}", result.error());
        return 1;
    }
    if(json) {
        print_json(*result);
        return result->failed.empty() ? 0 : 1;
    }
    driver::println("Indexed through the running clice server (pid {}).", endpoint.pid);
    if(!result->failed.empty()) {
        driver::println(
            "{} translation unit{} failed to index (see the server log); the index is partial:",
            result->failed.size(),
            plural_s(result->failed.size()));
        for(auto& path: result->failed) {
            driver::println("  {}", path);
        }
        return 1;
    }
    return 0;
}

int run_indexing(Spelling root,
                 std::string configuration,
                 std::uint32_t workers,
                 bool json,
                 const char* self_path) {
    auto config = Config::load_from_workspace(CanonicalPath(root));
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        LOG_ERROR("{}", checked.error());
        return 1;
    }
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::probe_writer(cache_dir);
    switch(writer.state) {
        case index::WriterProbe::State::Free: break;
        case index::WriterProbe::State::Server:
            return run_indexing_via_server(writer.endpoint,
                                           resolve_configuration(config, configuration),
                                           json);
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
        driver::println(
            stderr,
            "progress {}/{} units, {} failed, {:.0f}s elapsed",
            progress.completed,
            progress.total,
            progress.failed,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    };

    auto result = run_batch_index({
        .root = std::move(root),
        .config = std::move(config),
        .configuration = std::move(configuration),
        .workers = workers,
        .self_path = self_path,
        .on_progress = report_progress,
    });
    if(json) {
        print_json(result);
        return result.exit_code;
    }
    if(result.interrupted) {
        if(result.unsaved) {
            driver::println(
                "Indexing interrupted; part of the progress could not be persisted "
                "(see the log). Rerun `clice index` to resume.");
        } else {
            driver::println("Indexing interrupted; progress saved. Rerun `clice index` to resume.");
        }
        return result.exit_code;
    }
    if(!result.completed) {
        if(!result.log_dir.empty()) {
            driver::println("Session log: {}", result.log_dir);
        }
        return result.exit_code;
    }
    driver::println("Indexed {} translation unit{} in {:.1f}s: {} file shard{} ({}), {} symbol{}.",
                    result.indexed_tus,
                    plural_s(result.indexed_tus),
                    result.seconds,
                    result.shard_count,
                    plural_s(result.shard_count),
                    format_bytes(result.shard_bytes),
                    result.symbol_count,
                    plural_s(result.symbol_count));
    if(result.standalone_headers != 0) {
        driver::println(
            "The index holds {} header{} indexed standalone under borrowed compile "
            "commands.",
            result.standalone_headers,
            plural_s(result.standalone_headers));
    }
    if(!result.compiled_with_errors.empty()) {
        driver::println("{} translation unit{} compiled with errors; the index holds what parsed:",
                        result.compiled_with_errors.size(),
                        plural_s(result.compiled_with_errors.size()));
        for(auto& [unit, errors]: result.compiled_with_errors) {
            auto& first = errors.front();
            driver::println("  {}", unit);
            driver::println("    {}{}",
                            location(first.file, first.line, first.column),
                            first.message);
            if(errors.size() > 1) {
                driver::println("    and {} more error{}",
                                errors.size() - 1,
                                plural_s(errors.size() - 1));
            }
        }
    }
    if(!result.failed.empty()) {
        driver::println(
            "{} translation unit{} failed to index (see the log); the index is partial:",
            result.failed.size(),
            plural_s(result.failed.size()));
        for(auto& path: result.failed) {
            driver::println("  {}", path);
        }
    }
    if(result.unsaved) {
        driver::println("Part of the index could not be persisted (see the log).");
    }
    if(!result.log_dir.empty()) {
        driver::println("Session log: {}", result.log_dir);
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
            driver::println("  {:>8}  {:>9}  {:>5.1f}%",
                            label(bucket),
                            counts[bucket],
                            total != 0 ? 100.0 * static_cast<double>(counts[bucket]) /
                                             static_cast<double>(total)
                                       : 0.0);
        }
    }
};

struct ShardStat {
    std::string path;
    std::uint64_t bytes = 0;
    std::size_t variants = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t relations = 0;
};

struct InstantiationStat {
    std::string path;
    std::uint64_t bytes = 0;
    std::size_t resolutions = 0;
    std::uint64_t records = 0;
    std::size_t keys = 0;
};

constexpr std::pair<index::IndexBlobKind, llvm::StringLiteral> stored_kinds[] = {
    {index::IndexBlobKind::Shard,          "file shards"         },
    {index::IndexBlobKind::Instantiations, "instantiation shards"},
    {index::IndexBlobKind::Manifest,       "manifests"           },
    {index::IndexBlobKind::Global,         "global"              },
    {index::IndexBlobKind::Search,         "search"              },
    {index::IndexBlobKind::CDB,            "cdb"                 },
    {index::IndexBlobKind::Artifacts,      "artifacts"           },
    {index::IndexBlobKind::Contexts,       "contexts"            },
};

/// The blobs of one kind stored in the database, whether the loaded index
/// uses them or not.
struct StoredBlobs {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
};

struct IndexStats {
    std::vector<ShardStat> shards;
    index::FieldBytes shard_fields;
    std::uint64_t shard_bytes = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t relations = 0;
    std::uint64_t global_bytes = 0;
    index::FieldBytes global_fields;
    std::uint64_t search_bytes = 0;
    index::FieldBytes search_fields;
    std::uint64_t manifest_bytes = 0;
    index::FieldBytes manifest_fields;
    std::uint64_t local_fanout = 0;
    Histogram references_per_symbol;
    Histogram name_lengths;
    Histogram variants_per_shard;

    std::vector<InstantiationStat> instantiations;
    index::FieldBytes instantiation_fields;
    std::uint64_t instantiation_bytes = 0;
    std::uint64_t resolutions = 0;
    std::uint64_t records = 0;
    std::uint64_t keys = 0;
    /// Keys whose instantiation resolves nothing in its file.
    std::uint64_t empty_keys = 0;
    /// (unit, file) pairs the manifests list, and the distinct uses the
    /// instantiation shards store for them.
    std::uint64_t unit_uses = 0;
    std::uint64_t stored_uses = 0;
    /// Per template, how many of its instantiations the shards key.
    llvm::DenseMap<index::SymbolHash, std::uint32_t> keys_per_template;

    std::optional<index::StorageUsage> usage;
    /// Parallel to stored_kinds.
    std::array<StoredBlobs, std::size(stored_kinds)> stored;
};

IndexStats collect_stats(Project& project) {
    IndexStats stats;
    auto& project_index = project.project_index;

    stats.shards.reserve(project.project_index.shards.size());
    for(auto& [path_id, shard]: project.project_index.shards) {
        ShardStat stat{.path = project.file_table.display(path_id),
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
        if(index::ShardBlob blob; index::deserialize_blob(shard.bytes(), blob)) {
            index::measure_fields(blob, stats.shard_fields);
        }
        stats.shards.push_back(stat);
    }
    std::ranges::sort(stats.shards, std::ranges::greater{}, &ShardStat::bytes);

    for(auto& [path_id, instantiations]: project_index.instantiations) {
        InstantiationStat stat{.path = project.file_table.display(path_id),
                               .bytes = instantiations.bytes().size(),
                               .resolutions = instantiations.resolutions().size(),
                               .keys = instantiations.keys().size()};
        instantiations.for_each_record([&](const index::InstantiationRecord&) {
            stat.records += 1;
            return true;
        });
        instantiations.for_each_key([&](const index::InstantiationShard::Key& key) {
            stats.empty_keys += key.resolution ? 0 : 1;
            stats.keys_per_template[key.pattern] += 1;
            return true;
        });
        if(index::InstantiationBlob blob; index::deserialize_blob(instantiations.bytes(), blob)) {
            index::measure_fields(blob, stats.instantiation_fields);
        }
        stats.instantiation_bytes += stat.bytes;
        stats.stored_uses += instantiations.uses().size();
        stats.resolutions += stat.resolutions;
        stats.records += stat.records;
        stats.keys += stat.keys;
        stats.instantiations.push_back(std::move(stat));
    }
    std::ranges::sort(stats.instantiations, std::ranges::greater{}, &InstantiationStat::bytes);

    project_index.for_each_symbol(
        [&](index::SymbolHash, const index::SymbolIdentity& symbol, std::uint32_t references) {
            stats.references_per_symbol.add(references);
            stats.name_lengths.add(symbol.name.size());
            return true;
        });
    for(auto& [tu, manifest]: project_index.manifests) {
        stats.local_fanout += manifest.local_fanout.size();
        stats.unit_uses += manifest.instantiations.size();
        if(auto blob = project.index_db->read(index::IndexBlobKind::Manifest,
                                              project_index.key_of(project.file_table, tu))) {
            stats.manifest_bytes += blob.buffer->getBufferSize();
            index::measure_manifest(blob.buffer->getBuffer(), stats.manifest_fields);
        }
    }
    auto& db = *project.index_db;
    if(auto blob = db.read(index::IndexBlobKind::Global, "global")) {
        stats.global_bytes = blob.buffer->getBufferSize();
        index::measure_global(blob.buffer->getBuffer(), stats.global_fields);
    }
    if(auto blob = db.read(index::IndexBlobKind::Search, "search")) {
        stats.search_bytes = blob.buffer->getBufferSize();
        index::measure_search(blob.buffer->getBuffer(), stats.search_fields);
    }

    stats.usage = db.usage();
    for(auto [i, kind]: llvm::enumerate(llvm::make_first_range(stored_kinds))) {
        auto& stored = stats.stored[i];
        db.for_each_key(kind, [&](llvm::StringRef key) {
            if(auto blob = db.read(kind, key)) {
                stored.count += 1;
                stored.bytes += blob.buffer->getBufferSize();
            }
        });
    }
    return stats;
}

void print_stats(const Project& project,
                 llvm::ArrayRef<Fid> dropped,
                 const IndexStats& stats,
                 std::uint32_t top) {
    auto& project_index = project.project_index;
    auto share = [](std::uint64_t bytes, std::uint64_t whole) {
        return whole != 0 ? 100.0 * static_cast<double>(bytes) / static_cast<double>(whole) : 0.0;
    };
    auto column = [&](llvm::StringRef name, std::uint64_t bytes, std::uint64_t whole) {
        driver::println("  {:<32} {:>10}  {:>5.1f}%",
                        name,
                        format_bytes(bytes),
                        share(bytes, whole));
    };

    auto configuration = project.build.active_configuration();
    driver::println("Index cache: {}", index::library_directory(*project.store, configuration));
    if(!configuration.empty()) {
        driver::println("Configuration: {}", configuration);
    }
    driver::println("Translation units: {} (manifests {}), {} internal symbols spanning files",
                    project_index.manifests.size(),
                    format_bytes(stats.manifest_bytes),
                    stats.local_fanout);
    driver::println("File shards: {} ({}), {} occurrences, {} relations",
                    stats.shards.size(),
                    format_bytes(stats.shard_bytes),
                    stats.occurrences,
                    stats.relations);
    driver::println(
        "Instantiation shards: {} ({}), {} resolutions, {} records, {} keys ({} resolve "
        "nothing), {} uses by units ({} stored)",
        stats.instantiations.size(),
        format_bytes(stats.instantiation_bytes),
        stats.resolutions,
        stats.records,
        stats.keys,
        stats.empty_keys,
        stats.unit_uses,
        stats.stored_uses);
    driver::println("Global symbols: {}, file versions: {}",
                    project_index.symbol_count(),
                    project.file_table.versions.size());
    driver::println("Search index: {} symbols ({}), {} merged since its build",
                    project.project_index.search_index.size(),
                    format_bytes(stats.search_bytes),
                    project.project_index.search_pending.size());
    if(!dropped.empty()) {
        driver::println(
            "Translation units pending reindex (stale or partially written): {}; "
            "run `clice index` to repair",
            dropped.size());
    }

    if(auto& usage = stats.usage) {
        auto pages = [&](std::uint64_t count) {
            return format_bytes(count * usage->page_size);
        };
        driver::println();
        driver::println("Database file: {} in {} pages", pages(usage->pages), pages(1));
        driver::println("  {:<24} {:>10}", "tree pages", pages(usage->tree_pages));
        driver::println("  {:<24} {:>10}", "large values", pages(usage->overflow_pages));
        driver::println("  {:<24} {:>10}", "free pages", pages(usage->free_pages));
    }
    driver::println();
    driver::println("Stored blobs by kind:");
    for(auto [i, label]: llvm::enumerate(llvm::make_second_range(stored_kinds))) {
        auto& stored = stats.stored[i];
        driver::println("  {:<24} {:>10}  {:>7}", label, format_bytes(stored.bytes), stored.count);
    }

    // Field by field, largest first; framing is what the fields leave of
    // the blobs' size.
    auto fields = [&](llvm::StringRef title, const index::FieldBytes& bytes, std::uint64_t whole) {
        auto sorted = bytes.fields;
        std::ranges::stable_sort(sorted, std::ranges::greater{}, [](const auto& field) {
            return field.second;
        });
        std::uint64_t measured = 0;
        driver::println();
        driver::println("{} by field ({}):", title, format_bytes(whole));
        for(auto& [name, size]: sorted) {
            measured += size;
            if(size != 0) {
                column(name, size, whole);
            }
        }
        column("(framing)", whole > measured ? whole - measured : 0, whole);
    };
    fields("File shards", stats.shard_fields, stats.shard_bytes);
    fields("Instantiation shards", stats.instantiation_fields, stats.instantiation_bytes);
    fields("Manifests", stats.manifest_fields, stats.manifest_bytes);
    fields("Global blob", stats.global_fields, stats.global_bytes);
    fields("Search blob", stats.search_fields, stats.search_bytes);

    driver::println();
    driver::println("Symbols by reference file count:");
    stats.references_per_symbol.print();
    driver::println();
    driver::println("Symbols by name length:");
    stats.name_lengths.print();
    driver::println();
    driver::println("File shards by variant count:");
    stats.variants_per_shard.print();

    driver::println();
    driver::println("Top {} file shards by size:", std::min<std::size_t>(top, stats.shards.size()));
    for(auto& stat: stats.shards | std::views::take(top)) {
        driver::println("  {:>10}  {:>4} variants  {:>9} occs  {:>9} rels  {}",
                        format_bytes(stat.bytes),
                        stat.variants,
                        stat.occurrences,
                        stat.relations,
                        stat.path);
    }

    driver::println();
    driver::println("Top {} instantiation shards by size:",
                    std::min<std::size_t>(top, stats.instantiations.size()));
    for(auto& stat: stats.instantiations | std::views::take(top)) {
        driver::println("  {:>10}  {:>6} resolutions  {:>8} records  {:>7} keys  {}",
                        format_bytes(stat.bytes),
                        stat.resolutions,
                        stat.records,
                        stat.keys,
                        stat.path);
    }

    std::vector<std::pair<std::uint32_t, index::SymbolHash>> templates;
    for(auto [pattern, count]: stats.keys_per_template) {
        templates.emplace_back(count, pattern);
    }
    std::ranges::sort(templates, std::ranges::greater{});
    driver::println();
    driver::println("Top {} templates by instantiations:",
                    std::min<std::size_t>(top, templates.size()));
    for(auto [count, pattern]: templates | std::views::take(top)) {
        auto identity = project_index.identity_of(pattern);
        driver::println("  {:>8}  {}", count, identity ? identity->name : llvm::StringRef("?"));
    }
}

/// Every file shard with its variant count, most variants first — the
/// machine-readable form for cross-run comparisons.
void print_variants(const IndexStats& stats) {
    auto by_variants = stats.shards;
    std::ranges::stable_sort(by_variants, std::ranges::greater{}, &ShardStat::variants);
    driver::println();
    driver::println("variants\tpath");
    for(auto& stat: by_variants) {
        driver::println("{}\t{}", stat.variants, stat.path);
    }
}

int run_stats(Project& project, llvm::ArrayRef<Fid> dropped, std::uint32_t top, bool variants) {
    if(project.project_index.manifests.empty() && project.project_index.shards.empty()) {
        driver::println("Index is empty; run `clice index` to build it.");
        return 0;
    }
    auto stats = collect_stats(project);
    print_stats(project, dropped, stats, top);
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
        {index::SymbolFlags::HasDefinition,      "HasDefinition"     },
        {index::SymbolFlags::Template,           "Template"          },
        {index::SymbolFlags::Specialization,     "Specialization"    },
        {index::SymbolFlags::Deprecated,         "Deprecated"        },
        {index::SymbolFlags::InlineNamespace,    "InlineNamespace"   },
        {index::SymbolFlags::Unnamed,            "Unnamed"           },
        {index::SymbolFlags::SpelledInMacro,     "SpelledInMacro"    },
        {index::SymbolFlags::SystemHeader,       "SystemHeader"      },
        {index::SymbolFlags::Completable,        "Completable"       },
        {index::SymbolFlags::Exported,           "Exported"          },
        {index::SymbolFlags::AnonymousScope,     "AnonymousScope"    },
        {index::SymbolFlags::AnonymousNamespace, "AnonymousNamespace"},
        {index::SymbolFlags::TemplateParameter,  "TemplateParameter" },
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

/// What the index records about every symbol `text`, a locator, names:
/// an id its symbol, whether a file places it or not; anything else what
/// the query commands find for it, a name every symbol spelling it whole.
int run_show_symbol(Project& project, CommandResolver& commands, llvm::StringRef text) {
    auto locator = index::SymbolQuery::parse(text);
    if(!locator) {
        LOG_ERROR("{}", locator.error());
        return 1;
    }
    index::IndexQuery query(project.project_index, project.file_table, nullptr, nullptr);
    std::vector<index::SymbolHash> matches;
    if(locator->handle) {
        matches.push_back(*locator->handle);
    } else {
        ContextsBlob saved;
        EditorContext contexts{project, commands, saved};
        query::Context ctx{.project = project, .contexts = contexts, .query = query};
        auto named = query::named_symbols(ctx, std::move(*locator));
        if(!named) {
            driver::println("{}: {}", std::string_view(text), named.error());
            return 1;
        }
        matches = std::move(*named);
    }
    int rc = 0;
    for(auto hash: matches) {
        auto info = query.symbol_info(hash);
        if(!info) {
            driver::println("{}: no table knows this hash", format_hash(hash));
            rc = 1;
            continue;
        }
        driver::println("symbol {}  kind={}  name={}  args={}  qualified={}  flags={}  form={}",
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
                driver::println("  parent {}: unknown", format_hash(parent));
                break;
            }
            driver::println("  parent {}  kind={}  name={}",
                            format_hash(parent),
                            kind_name(scope->kind),
                            scope->display_name());
            parent = scope->parent;
        }
        if(auto symbol = project.project_index.identity_of(hash)) {
            driver::println("  scope={}  file={}  reference files={}",
                            kota::meta::enum_name(symbol->scope, "External"),
                            symbol->file == index::no_file
                                ? "-"
                                : project.file_table.display(Fid{symbol->file}),
                            project.project_index.reference_count(hash));
        } else {
            driver::println("  scope=local (not in the global table)");
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
        for(auto& [path_id, shard]: project.project_index.shards) {
            auto count = [&](RelationKind kind, std::size_t Counts::* field) {
                shard.lookup(hash, kind, [&](const index::Relation&) {
                    per_file[project.file_table.display(path_id)].*field += 1;
                    return true;
                });
            };
            count(RelationKind::Definition, &Counts::definitions);
            count(RelationKind::Declaration, &Counts::declarations);
            count(RelationKind::Reference, &Counts::references);
        }
        for(auto& [path, counts]: per_file) {
            driver::println("  {}: definitions={} declarations={} references={}",
                            path,
                            counts.definitions,
                            counts.declarations,
                            counts.references);
        }
    }
    return rc;
}

int run_show_file(Project& project, llvm::StringRef argument) {
    auto path = inspected_path(project, argument);
    auto file = project.file_table.find(path);
    auto shard_it =
        file ? project.project_index.shards.find(*file) : project.project_index.shards.end();
    if(shard_it == project.project_index.shards.end()) {
        driver::println("No rows for {} in the index.",
                        project.file_table.display(CanonicalPath(path)));
        return 1;
    }
    auto& shard = shard_it->second;
    driver::println("file {}", project.file_table.display(*file));
    driver::println("  blob={}  content size={}  content hash={}",
                    format_bytes(shard.bytes().size()),
                    shard.content_size(),
                    format_hash(shard.content_hash()));

    // Which unit contributed which variant, from the manifests.
    std::map<std::uint64_t, std::vector<std::string>> contributors;
    if(auto it = project.project_index.contributions.find(*file);
       it != project.project_index.contributions.end()) {
        for(auto& [tu, hash]: it->second) {
            contributors[hash].push_back(project.file_table.display(tu));
        }
    }
    auto variants = shard.variants();
    driver::println("  variants={}", variants.size());
    for(auto hash: variants) {
        auto& units = contributors[hash];
        std::ranges::sort(units);
        driver::println("    {}  contributed by {} unit{}",
                        format_hash(hash),
                        units.size(),
                        plural_s(units.size()));
        for(auto& unit: units) {
            driver::println("      {}", unit);
        }
    }

    llvm::DenseSet<index::SymbolHash> symbols;
    std::size_t occurrences = 0;
    shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
        symbols.insert(occurrence.target);
        occurrences += 1;
        return true;
    });
    std::map<llvm::StringRef, std::size_t> by_kind;
    std::size_t relations = 0;
    shard.for_each_relation([&](index::SymbolHash symbol, const index::Relation& relation) {
        symbols.insert(symbol);
        if(relation.target_symbol != 0 && !RelationKind(relation.kind).carries_extent()) {
            symbols.insert(relation.target_symbol);
        }
        by_kind[kota::meta::enum_name(relation.kind, "Invalid")] += 1;
        relations += 1;
        return true;
    });
    std::vector<std::pair<index::SymbolHash, index::SymbolIdentity>> locals;
    shard.for_each_local_symbol([&](index::SymbolHash hash, const index::SymbolIdentity& identity) {
        locals.emplace_back(hash, identity);
        return true;
    });
    driver::println("  symbols={}  local symbols={}  occurrences={}  relations={}",
                    symbols.size(),
                    locals.size(),
                    occurrences,
                    relations);
    for(auto& [kind, count]: by_kind) {
        driver::println("    {}={}", kind, count);
    }
    for(auto& [hash, identity]: locals) {
        driver::println("    local {}  kind={}  name={}{}",
                        format_hash(hash),
                        kind_name(identity.kind),
                        identity.name,
                        identity.args);
    }
    return 0;
}

int run_show_tu(Project& project, llvm::StringRef argument) {
    auto path = inspected_path(project, argument);
    auto& files = project.file_table;
    auto tu = files.find(path);
    auto& project_index = project.project_index;
    auto manifest_it = tu ? project_index.manifests.find(*tu) : project_index.manifests.end();
    if(manifest_it == project_index.manifests.end()) {
        driver::println("No manifest for {} in the index.", files.display(CanonicalPath(path)));
        return 1;
    }
    auto& manifest = manifest_it->second;
    auto version_path = [&](VersionID fv) {
        return files.display(files.version(fv).fid);
    };
    auto unit = files.display(*tu);
    driver::println("translation unit {}", unit);
    driver::println("  built at {}  generation={}  content hash={}",
                    format_time(manifest.built_at),
                    manifest.global_gen,
                    format_hash(files.version(manifest.tu_fv).content_hash));

    driver::println("  contributions={}", manifest.contributions.size());
    for(auto& [fv, hash]: manifest.contributions) {
        driver::println("    {}  {}", format_hash(hash), version_path(fv));
    }

    // The include tree, children under their parent in node order; a
    // node's line is the directive's line in the file that includes it.
    driver::println("  include tree ({} node{}):",
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
                            ? unit
                            : version_path(VersionID{manifest.nodes[entry.parent].file});
        driver::println("    {:{}}{}  {} at {}:{}",
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
        driver::println("    {} node{} unreachable from the root (cyclic parents)",
                        manifest.nodes.size() - printed,
                        plural_s(manifest.nodes.size() - printed));
        return 1;
    }
    return 0;
}

/// Open the persisted index of the workspace and hand it to `run`, which
/// reads it.
template <typename Run>
int read_index(const WorkspaceOptions& where, Run run) {
    auto spelling = where.root();
    if(!workspace_exists(spelling)) {
        return 1;
    }
    auto configuration = where.configuration.tag();
    auto config = Config::load_from_workspace(CanonicalPath(spelling));
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        LOG_ERROR("{}", checked.error());
        return 1;
    }
    FileTable files;
    // Answers name files under the workspace as the command line does.
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    auto loaded =
        load_index(project, commands, std::move(config), configuration, /*with_build=*/false);
    if(!loaded) {
        return 1;
    }
    return run(project, commands, *loaded);
}

/// An operation reading the record of the one file it names.
template <typename Show>
CommandHandler show_file(std::string_view usage, Show show) {
    return operation<FileOptions>(usage, "index", Errors::Text, [show](const FileOptions& opts) {
        auto args = positional(opts.target.inputs.as_optional(), {"<PATH>"});
        if(!args) {
            return usage_error(Errors::Text, args.error());
        }
        return read_index(opts.where, [&](Project& project, CommandResolver&, LoadedIndex&) {
            return show(project, args->front());
        });
    });
}

CommandHandler build(const char* self_path) {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "stats",
        .description = "What the persisted index holds: its sizes and how its records spread",
        .run = operation<StatsOptions>(
            "clice index stats [OPTIONS]",
            "index",
            Errors::Text,
            [](const StatsOptions& opts) {
                return read_index(opts.where,
                                  [&](Project& project, CommandResolver&, LoadedIndex& loaded) {
                                      return run_stats(project,
                                                       loaded.dropped,
                                                       opts.limit.value_or(20),
                                                       static_cast<bool>(opts.variants));
                                  });
            }),
    });
    operations.push_back({
        .name = "symbol",
        .description = "What the persisted index records about symbols",
        .run = operation<SymbolOptions>(
            "clice index symbol <LOCATOR> [OPTIONS]",
            "index",
            Errors::Text,
            [](const SymbolOptions& opts) {
                auto args = positional(opts.target.inputs.as_optional(), {"<LOCATOR>"});
                if(!args) {
                    return usage_error(Errors::Text, args.error());
                }
                return read_index(opts.where,
                                  [&](Project& project, CommandResolver& commands, LoadedIndex&) {
                                      return run_show_symbol(project, commands, args->front());
                                  });
            }),
    });
    operations.push_back({
        .name = "file",
        .description = "A file's persisted records: variants, contributing units and counts",
        .run = show_file("clice index file <PATH> [OPTIONS]", run_show_file),
    });
    operations.push_back({
        .name = "unit",
        .description = "A translation unit's persisted manifest: include tree and contributions",
        .run = show_file("clice index unit <PATH> [OPTIONS]", run_show_tu),
    });
    auto index =
        operation<IndexOptions>("clice index [OPTIONS]",
                                "index",
                                Errors::Text,
                                [self_path](const IndexOptions& opts) {
                                    auto spelling = opts.where.root();
                                    if(!workspace_exists(spelling)) {
                                        return 1;
                                    }
                                    return run_indexing(std::move(spelling),
                                                        opts.where.configuration.tag(),
                                                        opts.jobs.jobs.value_or(0),
                                                        static_cast<bool>(opts.report.json),
                                                        self_path);
                                });
    return with_operations("", std::move(operations), Errors::Text, std::move(index));
}

[[maybe_unused]] const bool registered =
    register_command(Command::index,
                     {.description = "Index a workspace ahead of time", .build = build});

}  // namespace

}  // namespace clice::driver
