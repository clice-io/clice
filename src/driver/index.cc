module clice;

import :config.config;
import :driver.driver;
import :index.database;
import :index.format;
import :index.instantiation_shard;
import :index.project_index;
import :index.project_records;
import :index.search_index;
import :index.shard;
import :index.symbol_query;
import :index.tu_manifest;
import :index.writer_lock;
import :feature.query;
import :project.configuration;
import :project.index_reader;
import :project.project;
import :sched.batch;
import :server.control;
import :support.text;
import :support.timer;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;
    ProjectOptions where;
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

    ProjectOptions where;
    LogLevelOption log;
};

struct SymbolOptions {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;
    ProjectOptions where;
    LogLevelOption log;
};

struct FileOptions {
    kota::deco::decl::HelpOption help;
    FileTarget target;
    ProjectOptions where;
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
    // FIXME: the failed units come in the server's spelling of the workspace,
    // which differs from this command's when the server or the command reaches
    // the workspace through a symlink.
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

int run_indexing(Spelling root, std::string configuration, std::uint32_t workers, bool json) {
    auto config = Config::load_from_workspace(CanonicalPath(root));
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        LOG_ERROR("{}", checked.error());
        return 1;
    }
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::check_writer(cache_dir);
    switch(writer.state) {
        case index::WriterStatus::State::Free: break;
        case index::WriterStatus::State::Server:
            return run_indexing_via_server(writer.endpoint,
                                           resolve_configuration(config, configuration),
                                           json);
        case index::WriterStatus::State::Held: {
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
    if(result.damaged) {
        driver::println(
            "Indexed {} translation unit{} in {:.1f}s, but the index database is damaged (see the "
            "log): it is discarded, and the next `clice index` rebuilds the index.",
            result.indexed_tus,
            plural_s(result.indexed_tus),
            result.seconds);
    } else {
        driver::println(
            "Indexed {} translation unit{} in {:.1f}s: {} file shard{} ({}), {} symbol{}.",
            result.indexed_tus,
            plural_s(result.indexed_tus),
            result.seconds,
            result.shard_count,
            plural_s(result.shard_count),
            format_bytes(result.shard_bytes),
            result.symbol_count,
            plural_s(result.symbol_count));
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
    if(!result.too_big.empty()) {
        driver::println(
            "{} translation unit{} did not fit in memory, even run alone; the index is partial:",
            result.too_big.size(),
            plural_s(result.too_big.size()));
        for(auto& [unit, memory]: result.too_big) {
            driver::println("  {} (reached {})", unit, format_bytes(memory));
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

constexpr std::pair<index::BlobKind, llvm::StringLiteral> stored_kinds[] = {
    {index::BlobKind::Shard,              "file shards"         },
    {index::BlobKind::InstantiationShard, "instantiation shards"},
    {index::BlobKind::TUManifest,         "manifests"           },
    {index::BlobKind::Symbol,             "symbols"             },
    {index::BlobKind::SharedLocal,        "shared locals"       },
    {index::BlobKind::File,               "files"               },
    {index::BlobKind::Dependents,         "dependents"          },
    {index::BlobKind::Internals,          "internal names"      },
    {index::BlobKind::Path,               "paths"               },
    {index::BlobKind::Version,            "versions"            },
    {index::BlobKind::Header,             "header"              },
    {index::BlobKind::SearchSegment,      "search segments"     },
    {index::BlobKind::CommandSnapshot,    "cdb"                 },
    {index::BlobKind::ArtifactRecords,    "artifacts"           },
    {index::BlobKind::ContextChoices,     "contexts"            },
};

/// The blobs of one kind stored in the database, whether the loaded index
/// uses them or not.
struct StoredBlobs {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
};

struct Stats {
    std::vector<ShardStat> shards;
    index::FieldBytes shard_fields;
    std::uint64_t shard_bytes = 0;
    std::uint64_t occurrences = 0;
    std::uint64_t relations = 0;
    std::uint64_t symbol_bytes = 0;
    index::FieldBytes symbol_fields;
    std::uint64_t search_bytes = 0;
    index::FieldBytes search_fields;
    std::uint64_t manifest_bytes = 0;
    index::FieldBytes manifest_fields;
    std::uint64_t units = 0;
    std::uint64_t shared_locals = 0;
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

    std::optional<index::PageUsage> usage;
    /// Parallel to stored_kinds.
    std::array<StoredBlobs, std::size(stored_kinds)> stored;
};

Stats collect_stats(Project& project) {
    Stats stats;
    auto& project_index = project.index;

    project_index.for_each_indexed_file([&](Fid path_id) {
        auto* shard = project_index.shard(path_id);
        if(!shard) {
            return;
        }
        ShardStat stat{.path = project.file_table.display(path_id),
                       .bytes = shard->bytes().size(),
                       .variants = shard->variants().size()};
        shard->for_each_occurrence([&](const index::Occurrence&) {
            stat.occurrences += 1;
            return true;
        });
        shard->for_each_relation([&](index::SymbolHash, const index::Relation&) {
            stat.relations += 1;
            return true;
        });
        stats.shard_bytes += stat.bytes;
        stats.occurrences += stat.occurrences;
        stats.relations += stat.relations;
        stats.variants_per_shard.add(stat.variants);
        index::measure_blob<index::ShardBlob>(shard->bytes(), stats.shard_fields);
        stats.shards.push_back(stat);

        auto* instantiations = project_index.instantiation_shard(path_id);
        if(!instantiations) {
            return;
        }
        InstantiationStat held{.path = project.file_table.display(path_id),
                               .bytes = instantiations->bytes().size(),
                               .resolutions = instantiations->resolutions().size(),
                               .keys = instantiations->keys().size()};
        instantiations->for_each_record([&](const index::InstantiationRecord&) {
            held.records += 1;
            return true;
        });
        instantiations->for_each_key([&](const index::InstantiationShard::Key& key) {
            stats.empty_keys += key.resolution ? 0 : 1;
            stats.keys_per_template[key.pattern] += 1;
            return true;
        });
        index::measure_blob<index::InstantiationShardBlob>(instantiations->bytes(),
                                                           stats.instantiation_fields);
        stats.instantiation_bytes += held.bytes;
        stats.stored_uses += instantiations->uses().size();
        stats.resolutions += held.resolutions;
        stats.records += held.records;
        stats.keys += held.keys;
        stats.instantiations.push_back(std::move(held));
    });
    ranges::sort(stats.shards, ranges::greater{}, &ShardStat::bytes);
    ranges::sort(stats.instantiations, ranges::greater{}, &InstantiationStat::bytes);

    project_index.for_each_symbol(
        [&](index::SymbolHash, const index::SymbolView& symbol, std::uint32_t references) {
            stats.references_per_symbol.add(references);
            stats.name_lengths.add(symbol.name.size());
            return true;
        });
    project_index.for_each_unit([&](Fid tu) {
        if(auto manifest = project_index.manifest(tu)) {
            stats.units += 1;
            stats.shared_locals += manifest->shared_locals.size();
            stats.unit_uses += manifest->instantiations.size();
        }
    });
    auto& db = *project.database;
    db.for_each(index::BlobKind::TUManifest, [&](llvm::StringRef, index::ReadBlob blob) {
        stats.manifest_bytes += blob.buffer->getBufferSize();
        index::measure_blob<index::TUManifestBlob>(blob.buffer->getBuffer(), stats.manifest_fields);
        return true;
    });
    db.for_each(index::BlobKind::Symbol, [&](llvm::StringRef, index::ReadBlob blob) {
        stats.symbol_bytes += blob.buffer->getBufferSize();
        index::measure_symbol(blob.buffer->getBuffer(), stats.symbol_fields);
        return true;
    });
    db.for_each(index::BlobKind::SearchSegment, [&](llvm::StringRef, index::ReadBlob blob) {
        stats.search_bytes += blob.buffer->getBufferSize();
        index::measure_blob<index::SearchIndexBlob>(blob.buffer->getBuffer(), stats.search_fields);
        return true;
    });

    stats.usage = db.usage();
    for(auto [i, kind]: views::enumerate(views::keys(stored_kinds))) {
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

void print_stats(const Project& project, const Stats& stats, std::uint32_t top) {
    auto& project_index = project.index;
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
    driver::println("Index cache: {}",
                    index::database_directory(*project.store, project.database_key));
    if(!configuration.empty()) {
        driver::println("Configuration: {}", configuration);
    }
    driver::println("Translation units: {} (manifests {}), {} internal symbols spanning files",
                    stats.units,
                    format_bytes(stats.manifest_bytes),
                    stats.shared_locals);
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
    driver::println("Search index: {} symbols in {} run{} ({}), {} merged since the newest run",
                    project.index.search_index().size(),
                    project.index.search_index().run_count(),
                    plural_s(project.index.search_index().run_count()),
                    format_bytes(stats.search_bytes),
                    project.index.search_pending().size());
    if(project_index.damaged()) {
        driver::println("Warning: {}", damaged_index_error(project));
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
    for(auto [i, label]: views::enumerate(views::values(stored_kinds))) {
        auto& stored = stats.stored[i];
        driver::println("  {:<24} {:>10}  {:>7}", label, format_bytes(stored.bytes), stored.count);
    }

    // Field by field, largest first; framing is what the fields leave of
    // the blobs' size.
    auto fields = [&](llvm::StringRef title, const index::FieldBytes& bytes, std::uint64_t whole) {
        auto sorted = bytes.fields;
        ranges::stable_sort(sorted, ranges::greater{}, [](const auto& field) {
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
    fields("Symbol records", stats.symbol_fields, stats.symbol_bytes);
    fields("Search segments", stats.search_fields, stats.search_bytes);

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
    for(auto& stat: stats.shards | views::take(top)) {
        driver::println("  {:>10}  {:>4} variants  {:>9} occurrences  {:>9} relations  {}",
                        format_bytes(stat.bytes),
                        stat.variants,
                        stat.occurrences,
                        stat.relations,
                        stat.path);
    }

    driver::println();
    driver::println("Top {} instantiation shards by size:",
                    std::min<std::size_t>(top, stats.instantiations.size()));
    for(auto& stat: stats.instantiations | views::take(top)) {
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
    ranges::sort(templates, ranges::greater{});
    driver::println();
    driver::println("Top {} templates by instantiations:",
                    std::min<std::size_t>(top, templates.size()));
    for(auto [count, pattern]: templates | views::take(top)) {
        auto identity = project_index.find_symbol(pattern);
        driver::println("  {:>8}  {}", count, identity ? identity->name : llvm::StringRef("?"));
    }
}

/// Every file shard with its variant count, most variants first — the
/// machine-readable form for cross-run comparisons.
void print_variants(const Stats& stats) {
    auto by_variants = stats.shards;
    ranges::stable_sort(by_variants, ranges::greater{}, &ShardStat::variants);
    driver::println();
    driver::println("variants\tpath");
    for(auto& stat: by_variants) {
        driver::println("{}\t{}", stat.variants, stat.path);
    }
}

int run_stats(Project& project, std::uint32_t top, bool variants) {
    auto stats = collect_stats(project);
    if(stats.units == 0 && stats.shards.empty()) {
        driver::println("Index is empty; run `clice index` to build it.");
        return 0;
    }
    print_stats(project, stats, top);
    if(variants) {
        print_variants(stats);
    }
    return 0;
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
/// `clice query` finds for it, a name every symbol spelling it whole.
int run_show_symbol(IndexReader& reader, llvm::StringRef text) {
    auto locator = index::SymbolQuery::parse(text);
    if(!locator) {
        LOG_ERROR("{}", locator.error());
        return 1;
    }
    auto& project = reader.project;
    auto& query = reader.query;

    // Each match with the file it was found in: a symbol below external
    // scope is known to the files holding it.
    struct Match {
        index::SymbolHash hash;
        Fid anchor;
    };

    std::vector<Match> matches;
    auto ctx = reader.context();
    auto handle = locator->handle;
    auto paths = locator->paths;
    auto named = feature::query::named_symbols(ctx, std::move(*locator));
    if(named) {
        for(auto& located: *named) {
            matches.push_back({located.symbol.hash, located.site.file});
        }
    } else if(handle) {
        // An id names its symbol whether a file places it or not, its path
        // the file holding its entries.
        Fid anchor;
        if(!paths.empty()) {
            anchor = project.file_table.intern(
                Spelling(paths.front(), Spelling(project.config.project_root)));
        }
        matches.push_back({*handle, anchor});
    } else {
        driver::println("{}: {}", std::string_view(text), named.error());
        return 1;
    }
    int rc = 0;
    for(auto [hash, anchor]: matches) {
        auto info = query.symbol_info(hash, anchor);
        if(!info) {
            driver::println("{}: no table knows this hash", index::symbol_id(hash));
            rc = 1;
            continue;
        }
        driver::println("symbol {}  kind={}  name={}  args={}  qualified={}  flags={}  form={}",
                        index::symbol_id(hash),
                        kind_name(info->kind),
                        info->name,
                        info->args,
                        query.qualified_name(hash, anchor),
                        flag_names(info->flags),
                        kota::meta::enum_name(index::name_form(info->flags), "Other"));
        // Persisted parents can form a cycle: the records only reject
        // reserved values.
        llvm::DenseSet<index::SymbolHash> visited{hash};
        for(auto parent = info->parent; parent != 0 && visited.insert(parent).second;) {
            auto scope = query.symbol_info(parent, anchor);
            if(!scope) {
                driver::println("  parent {}: unknown", index::symbol_id(parent));
                break;
            }
            driver::println("  parent {}  kind={}  name={}",
                            index::symbol_id(parent),
                            kind_name(scope->kind),
                            scope->display_name());
            parent = scope->parent;
        }
        if(auto symbol = project.index.find_symbol(hash)) {
            driver::println("  scope={}  file={}  reference files={}",
                            kota::meta::enum_name(symbol->scope, "External"),
                            symbol->file == index::no_file
                                ? "-"
                                : project.file_table.display(Fid{symbol->file}),
                            project.index.reference_count(hash));
        } else {
            driver::println("  scope=local (no symbol record)");
        }

        struct Counts {
            std::size_t definitions = 0;
            std::size_t declarations = 0;
            std::size_t references = 0;
        };

        // Straight from every shard: the query's fan-out follows the symbol
        // records' reference files, which a file-local symbol has none of.
        std::map<std::string, Counts> per_file;
        project.index.for_each_indexed_file([&](Fid path_id) {
            auto* indexed = project.index.shard(path_id);
            if(!indexed) {
                return;
            }
            auto& shard = *indexed;
            auto count = [&](RelationKind kind, std::size_t Counts::* field) {
                shard.relations_of(hash, kind, [&](const index::Relation&) {
                    per_file[project.file_table.display(path_id)].*field += 1;
                    return true;
                });
            };
            count(RelationKind::Definition, &Counts::definitions);
            count(RelationKind::Declaration, &Counts::declarations);
            count(RelationKind::Reference, &Counts::references);
        });
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
    auto path = argument_path(project, argument);
    // Interning only names the file: the index reads its records by name.
    auto file = project.file_table.intern(path);
    auto* found = project.index.shard(file);
    if(!found) {
        driver::println("No entries for {} in the index.",
                        project.file_table.display(CanonicalPath(path)));
        return 1;
    }
    auto& shard = *found;
    driver::println("file {}", project.file_table.display(file));
    driver::println("  blob={}  content size={}  content hash={}",
                    format_bytes(shard.bytes().size()),
                    shard.content_size(),
                    format_hash(shard.content_hash()));

    // Which unit contributed which variant, from the dependents record.
    std::map<std::uint64_t, std::vector<std::string>> contributors;
    project.index.for_each_contribution(file, [&](Fid tu, std::uint64_t hash) {
        contributors[hash].push_back(project.file_table.display(tu));
    });
    auto variants = shard.variants();
    driver::println("  variants={}", variants.size());
    for(auto hash: variants) {
        auto& units = contributors[hash];
        ranges::sort(units);
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
        if(relation.target != 0 && !RelationKind(relation.kind).carries_extent()) {
            symbols.insert(relation.target);
        }
        by_kind[kota::meta::enum_name(relation.kind, "Invalid")] += 1;
        relations += 1;
        return true;
    });
    std::vector<std::pair<index::SymbolHash, index::SymbolView>> locals;
    shard.for_each_local_symbol([&](index::SymbolHash hash, const index::SymbolView& identity) {
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
                        index::symbol_id(hash),
                        kind_name(identity.kind),
                        identity.name,
                        identity.args);
    }
    return 0;
}

int run_show_tu(Project& project, llvm::StringRef argument) {
    auto path = argument_path(project, argument);
    auto& files = project.file_table;
    auto tu = files.intern(path);
    auto& project_index = project.index;
    auto found = project_index.manifest(tu);
    if(!found) {
        driver::println("No manifest for {} in the index.", files.display(CanonicalPath(path)));
        return 1;
    }
    auto& manifest = *found;
    auto version_path = [&](VersionID version) {
        return files.display(files.version(version).fid);
    };
    auto unit = files.display(tu);
    driver::println("translation unit {}", unit);
    driver::println("  built at {}  content hash={}",
                    format_time(manifest.built_at),
                    format_hash(files.version(manifest.tu_version).content_hash));

    driver::println("  contributions={}", manifest.contributions.size());
    for(auto& [version, hash]: manifest.contributions) {
        driver::println("    {}  {}", format_hash(hash), version_path(version));
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

/// Open the persisted index of the workspace as the writer loads it, every
/// file served as it was indexed, and hand it to `run`, which reads it.
template <typename Run>
int read_index(const ProjectOptions& where, Run run) {
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
    auto reader = IndexReader::open(spelling,
                                    std::move(config),
                                    configuration,
                                    {.load = IndexLoad::Saved, .check_disk = false});
    if(!reader) {
        return 1;
    }
    auto rc = run(*reader);
    // A damaged record reads as missing, its log line the only trace: what
    // was printed is no answer automation may take as one.
    return reader->project.index.damaged() ? 1 : rc;
}

/// An operation reading the record of the one file it names.
template <typename Show>
CommandHandler show_file(std::string_view usage, Show show) {
    return operation<FileOptions>(usage, "index", Errors::Text, [show](const FileOptions& opts) {
        return read_index(opts.where, [&](IndexReader& reader) {
            return show(reader.project, *opts.target.path);
        });
    });
}

CommandHandler build() {
    std::vector<Operation> operations;
    operations.push_back({
        .name = "stats",
        .description = "What the persisted index holds: its sizes and how its records spread",
        .run = operation<StatsOptions>("clice index stats [OPTIONS]",
                                       "index",
                                       Errors::Text,
                                       [](const StatsOptions& opts) {
                                           return read_index(opts.where, [&](IndexReader& reader) {
                                               return run_stats(reader.project,
                                                                opts.limit.value_or(20),
                                                                static_cast<bool>(opts.variants));
                                           });
                                       }),
    });
    operations.push_back({
        .name = "symbol",
        .description = "What the persisted index records about symbols",
        .run = operation<SymbolOptions>("clice index symbol <LOCATOR> [OPTIONS]",
                                        "index",
                                        Errors::Text,
                                        [](const SymbolOptions& opts) {
                                            return read_index(opts.where, [&](IndexReader& reader) {
                                                return run_show_symbol(reader,
                                                                       *opts.target.locator);
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
        operation<Options>("clice index [OPTIONS]", "index", Errors::Text, [](const Options& opts) {
            auto spelling = opts.where.root();
            if(!workspace_exists(spelling)) {
                return 1;
            }
            return run_indexing(std::move(spelling),
                                opts.where.configuration.tag(),
                                opts.jobs.jobs.value_or(0),
                                static_cast<bool>(opts.report.json));
        });
    return with_operations("", std::move(operations), Errors::Text, std::move(index));
}

[[maybe_unused]] const bool registered =
    register_command(Command::index,
                     {.description = "Index a workspace ahead of time", .build = build});

}  // namespace

}  // namespace clice::driver
