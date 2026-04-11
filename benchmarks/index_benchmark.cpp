/// Benchmark for TUIndex build, MergedIndex merge, and query performance.
///
/// Usage:
///   index_benchmark [OPTIONS] <compile_commands.json>
///
/// Example:
///   ./build/RelWithDebInfo/bin/index_benchmark \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <print>
#include <sstream>
#include <thread>
#include <vector>

#include "command/command.h"
#include "compile/compilation.h"
#include "index/merged_index.h"
#include "index/project_index.h"
#include "index/tu_index.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"

using namespace clice;

struct Options {
    DecoKV(names = {"--log-level"}; help = "Log level"; required = false;)
    <std::string> log_level = "off";

    DecoKV(names = {"--limit"}; help = "Max files to index (0 = all)"; required = false;)
    <int> limit = 100;

    DecoKV(names = {"--jobs", "-j"}; help = "Parallel compile jobs (0 = nproc)"; required = false;)
    <int> jobs = 0;

    DecoFlag(names = {"-h", "--help"}; help = "Show help"; required = false;)
    help;

    DecoInput(meta_var = "CDB"; help = "Path to compile_commands.json"; required = false;)
    <std::string> cdb_path;
};

struct Timer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    double ms() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    void reset() {
        start = std::chrono::steady_clock::now();
    }
};

struct IndexStats {
    std::string file;
    double compile_ms = 0;
    double build_index_ms = 0;
    double serialize_ms = 0;
    std::size_t symbols = 0;
    std::size_t occurrences = 0;
    std::size_t relations = 0;
    std::size_t serialized_bytes = 0;
    std::size_t file_count = 0;
};

struct IndexResult {
    IndexStats stats;
    index::TUIndex tu_index;
};

struct MergeStats {
    std::size_t total_files = 0;
    std::size_t total_occurrences = 0;
    std::size_t total_relations = 0;
    double merge_ms = 0;
    double serialize_ms = 0;
    std::size_t total_serialized_bytes = 0;
    std::size_t shard_count = 0;
};

void print_size(std::size_t bytes) {
    if(bytes < 1024)
        std::print("{}B", bytes);
    else if(bytes < 1024 * 1024)
        std::print("{:.1f}KB", bytes / 1024.0);
    else
        std::print("{:.1f}MB", bytes / (1024.0 * 1024.0));
}

/// Per-task: compile one file, build TUIndex, serialize, collect stats.
/// Everything here is thread-local, no shared state.
static std::optional<IndexResult> index_one_file(CompilationDatabase& cdb,
                                                 const CompilationEntry& entry) {
    auto file = cdb.resolve_path(entry.file);
    auto cmds = cdb.lookup(file);
    if(cmds.empty())
        return std::nullopt;

    auto& cmd = cmds[0];
    auto argv_vec = cmd.to_argv();

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    cp.directory = cmd.resolved.directory;
    for(auto* arg: argv_vec)
        cp.arguments.push_back(arg);

    IndexResult result;
    result.stats.file = std::string(file);

    Timer t;
    auto unit = compile(cp);
    result.stats.compile_ms = t.ms();

    if(!unit.completed())
        return std::nullopt;

    t.reset();
    result.tu_index = index::TUIndex::build(unit);
    result.stats.build_index_ms = t.ms();

    result.stats.symbols = result.tu_index.symbols.size();
    result.stats.file_count = result.tu_index.file_indices.size() + 1;
    for(auto& [fid, fi]: result.tu_index.file_indices) {
        result.stats.occurrences += fi.occurrences.size();
        for(auto& [_, rels]: fi.relations)
            result.stats.relations += rels.size();
    }
    result.stats.occurrences += result.tu_index.main_file_index.occurrences.size();
    for(auto& [_, rels]: result.tu_index.main_file_index.relations)
        result.stats.relations += rels.size();

    t.reset();
    std::string serialized;
    llvm::raw_string_ostream os(serialized);
    result.tu_index.serialize(os);
    result.stats.serialize_ms = t.ms();
    result.stats.serialized_bytes = serialized.size();

    return result;
}

int main(int argc, const char** argv) {
    auto args = deco::util::argvify(argc, argv);
    auto parsed = deco::cli::parse<Options>(args);
    if(!parsed.has_value()) {
        std::println(stderr, "Error: {}", parsed.error().message);
        return 1;
    }

    auto& opts = parsed->options;
    if(opts.help.value_or(false) || !opts.cdb_path.has_value()) {
        std::ostringstream oss;
        deco::cli::write_usage_for<Options>(oss, "index_benchmark [OPTIONS] <cdb>");
        std::print("{}", oss.str());
        return 0;
    }

    auto level = spdlog::level::from_str(*opts.log_level);
    clice::logging::options.level = level;
    clice::logging::stderr_logger("index_benchmark", clice::logging::options);

    auto num_jobs = static_cast<unsigned>(*opts.jobs);
    if(num_jobs == 0)
        num_jobs = std::thread::hardware_concurrency();
    if(num_jobs == 0)
        num_jobs = 4;

    // Load CDB.
    CompilationDatabase cdb;
    auto count = cdb.load(*opts.cdb_path);
    std::println("CDB: {} entries from {}", count, *opts.cdb_path);

    auto entries = cdb.get_entries();
    auto limit = static_cast<std::size_t>(*opts.limit);
    if(limit > 0 && limit < entries.size())
        entries = entries.take_front(limit);
    std::println("Indexing {} files with {} threads...\n", entries.size(), num_jobs);

    // Phase 1: Parallel compile & build TUIndex.
    // Each thread picks tasks from a shared atomic counter.
    // Results are written to a pre-sized vector (one slot per entry).
    std::vector<std::optional<IndexResult>> results(entries.size());
    std::atomic<std::size_t> next_task{0};
    std::atomic<std::size_t> completed{0};

    Timer phase1_wall;

    {
        std::vector<std::jthread> workers;
        workers.reserve(num_jobs);
        for(unsigned i = 0; i < num_jobs; i++) {
            workers.emplace_back([&] {
                while(true) {
                    auto idx = next_task.fetch_add(1);
                    if(idx >= entries.size())
                        break;
                    results[idx] = index_one_file(cdb, entries[idx]);
                    auto done = completed.fetch_add(1) + 1;
                    if(done % 100 == 0)
                        std::println("  ... {}/{} done", done, entries.size());
                }
            });
        }
    }

    double phase1_wall_ms = phase1_wall.ms();

    // Collect results on main thread.
    std::vector<IndexStats> all_stats;
    std::vector<std::pair<std::string, index::TUIndex>> tu_indices;
    all_stats.reserve(entries.size());
    tu_indices.reserve(entries.size());

    double total_compile_ms = 0;
    double total_build_ms = 0;
    double total_serialize_ms = 0;
    std::size_t total_serialized = 0;

    for(auto& r: results) {
        if(!r)
            continue;
        total_compile_ms += r->stats.compile_ms;
        total_build_ms += r->stats.build_index_ms;
        total_serialize_ms += r->stats.serialize_ms;
        total_serialized += r->stats.serialized_bytes;
        tu_indices.emplace_back(std::move(r->stats.file), std::move(r->tu_index));
        all_stats.push_back(std::move(r->stats));
    }

    std::println("=== Phase 1: Compile & Build TUIndex ({} threads) ===", num_jobs);
    std::println("  Files indexed:    {}", all_stats.size());
    std::println("  Wall time:        {:.0f}ms", phase1_wall_ms);
    std::println("  Total compile:    {:.0f}ms (sum of per-thread)", total_compile_ms);
    std::println("  Total build idx:  {:.0f}ms (sum of per-thread)", total_build_ms);
    std::println("  Total serialize:  {:.0f}ms (sum of per-thread)", total_serialize_ms);
    std::print("  Total TUIndex size: ");
    print_size(total_serialized);
    std::println("");

    // Size distribution.
    if(!all_stats.empty()) {
        std::vector<std::size_t> sizes;
        std::vector<double> compile_times;
        std::vector<double> build_times;
        for(auto& s: all_stats) {
            sizes.push_back(s.serialized_bytes);
            compile_times.push_back(s.compile_ms);
            build_times.push_back(s.build_index_ms);
        }
        std::ranges::sort(sizes);
        std::ranges::sort(compile_times);
        std::ranges::sort(build_times);
        auto p = [](auto& v, double pct) {
            return v[static_cast<std::size_t>(v.size() * pct)];
        };

        std::println("\n  TUIndex size distribution:");
        std::print("    p50=");
        print_size(p(sizes, 0.5));
        std::print("  p90=");
        print_size(p(sizes, 0.9));
        std::print("  p99=");
        print_size(p(sizes, 0.99));
        std::print("  max=");
        print_size(sizes.back());
        std::println("");

        std::println("  Compile time distribution:");
        std::println("    p50={:.0f}ms  p90={:.0f}ms  p99={:.0f}ms  max={:.0f}ms",
                     p(compile_times, 0.5),
                     p(compile_times, 0.9),
                     p(compile_times, 0.99),
                     compile_times.back());

        std::println("  Build index time distribution:");
        std::println("    p50={:.1f}ms  p90={:.1f}ms  p99={:.1f}ms  max={:.1f}ms",
                     p(build_times, 0.5),
                     p(build_times, 0.9),
                     p(build_times, 0.99),
                     build_times.back());
    }

    // Phase 2: Merge into MergedIndex shards (main thread).
    std::println("\n=== Phase 2: Merge into MergedIndex ===");

    index::ProjectIndex project_index;
    llvm::DenseMap<std::uint32_t, index::MergedIndex> merged;
    MergeStats merge_stats;
    Timer merge_timer;

    llvm::DenseMap<std::uint32_t, std::string> file_contents;

    auto read_content = [&](std::uint32_t path_id, llvm::StringRef path) -> llvm::StringRef {
        auto it = file_contents.find(path_id);
        if(it != file_contents.end())
            return it->second;
        auto buf = llvm::MemoryBuffer::getFile(path);
        if(!buf)
            return {};
        auto [inserted, _] = file_contents.try_emplace(path_id, (*buf)->getBuffer().str());
        return inserted->second;
    };

    // Pre-compute SHA256 hashes (simulates offloading to worker threads).
    // With cached hash, this warms the cache so MergedIndex::merge won't recompute.
    Timer hash_timer;
    std::size_t hash_count = 0;
    for(auto& [file, tu_index]: tu_indices) {
        for(auto& [fid, fi]: tu_index.file_indices) {
            fi.hash();
            hash_count++;
        }
        tu_index.main_file_index.hash();
        hash_count++;
    }
    double precompute_hash_ms = hash_timer.ms();

    // Merge with both optimizations:
    // 1. SHA256 already cached from above (MergedIndex::merge uses cached values)
    // 2. Selective ProjectIndex update (skip symbols only in cache-hit files)
    double project_merge_full_ms = 0;
    double project_merge_selective_ms = 0;
    double merged_index_merge_ms = 0;
    std::size_t cache_hits = 0;
    std::size_t cache_misses = 0;

    for(auto& [file, tu_index]: tu_indices) {
        auto& graph = tu_index.graph;
        auto main_tu_path_id = static_cast<std::uint32_t>(graph.paths.size() - 1);

        // First: MergedIndex merges to find out which files are new.
        Bitmap new_file_ids;
        Timer mt;
        for(auto& [fid, fi]: tu_index.file_indices) {
            auto tu_path_id = graph.path_id(fid);
            auto global_path_id = project_index.path_pool.path_id(graph.paths[tu_path_id]);
            auto content = read_content(global_path_id, graph.paths[tu_path_id]);
            auto include_id = graph.include_location_id(fid);
            bool hit = merged[global_path_id].merge(global_path_id, include_id, fi, content);
            if(hit) {
                cache_hits++;
            } else {
                cache_misses++;
                new_file_ids.add(tu_path_id);
            }
            merge_stats.total_files++;
        }

        // Main file is always new.
        new_file_ids.add(main_tu_path_id);
        auto global_main_id = project_index.path_pool.path_id(graph.paths[main_tu_path_id]);
        auto content = read_content(global_main_id, graph.paths[main_tu_path_id]);
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        std::vector<index::IncludeLocation> remapped_locs;
        for(auto& loc: graph.locations) {
            auto remapped = loc;
            remapped.path_id = project_index.path_pool.path_id(graph.paths[loc.path_id]);
            remapped_locs.push_back(remapped);
        }
        merged[global_main_id].merge(global_main_id,
                                     now,
                                     std::move(remapped_locs),
                                     tu_index.main_file_index,
                                     content);
        cache_misses++;
        merged_index_merge_ms += mt.ms();

        // Then: selective ProjectIndex merge (only symbols in new files).
        Timer pt;
        project_index.merge(tu_index, new_file_ids);
        project_merge_selective_ms += pt.ms();
    }

    merge_stats.merge_ms = merge_timer.ms();
    merge_stats.shard_count = merged.size();

    // Serialize all shards to measure size.
    merge_timer.reset();
    for(auto& [path, shard]: merged) {
        std::string buf;
        llvm::raw_string_ostream os(buf);
        shard.serialize(os);
        merge_stats.total_serialized_bytes += buf.size();
    }
    merge_stats.serialize_ms = merge_timer.ms();

    std::println("  Shards:           {}", merge_stats.shard_count);
    std::println("  SHA256 precompute (offloadable): {:.0f}ms ({} hashes)",
                 precompute_hash_ms,
                 hash_count);
    std::println("  MergedIndex merge (cached hash): {:.0f}ms (hit: {}, miss: {})",
                 merged_index_merge_ms,
                 cache_hits,
                 cache_misses);
    std::println("  ProjectIndex merge (selective):  {:.0f}ms", project_merge_selective_ms);
    std::println("  Main-thread total:  {:.0f}ms ({:.1f}ms/TU)",
                 merged_index_merge_ms + project_merge_selective_ms,
                 (merged_index_merge_ms + project_merge_selective_ms) / tu_indices.size());
    std::println("  Serialize time:     {:.0f}ms", merge_stats.serialize_ms);
    std::print("  Total size:       ");
    print_size(merge_stats.total_serialized_bytes);
    std::println("");

    // Shard size distribution.
    {
        std::vector<std::size_t> shard_sizes;
        shard_sizes.reserve(merged.size());
        for(auto& [path, shard]: merged) {
            std::string buf;
            llvm::raw_string_ostream os(buf);
            shard.serialize(os);
            shard_sizes.push_back(buf.size());
        }
        std::ranges::sort(shard_sizes);
        if(!shard_sizes.empty()) {
            auto p = [&](double pct) {
                return shard_sizes[static_cast<std::size_t>(shard_sizes.size() * pct)];
            };
            std::println("\n  Shard size distribution:");
            std::print("    p50=");
            print_size(p(0.5));
            std::print("  p90=");
            print_size(p(0.9));
            std::print("  p99=");
            print_size(p(0.99));
            std::print("  max=");
            print_size(shard_sizes.back());
            std::println("");
        }
    }

    // Phase 3: Query benchmark.
    std::println("\n=== Phase 3: Query Benchmark ===");

    // Collect some symbol hashes to query.
    llvm::SmallVector<index::SymbolHash> sample_hashes;
    for(auto& [file, tu_index]: tu_indices) {
        for(auto& [hash, sym]: tu_index.symbols) {
            sample_hashes.push_back(hash);
            if(sample_hashes.size() >= 1000)
                break;
        }
        if(sample_hashes.size() >= 1000)
            break;
    }

    // Occurrence lookup by offset (raw, no position conversion).
    {
        std::size_t hit_count = 0;
        Timer t;
        constexpr int rounds = 10;
        for(int r = 0; r < rounds; r++) {
            for(auto& [path, shard]: merged) {
                for(std::uint32_t offset = 0; offset < 1000; offset += 50) {
                    shard.lookup(offset, [&](const index::Occurrence&) {
                        hit_count++;
                        return true;
                    });
                }
            }
        }
        auto elapsed = t.ms();
        auto queries = merged.size() * 20 * rounds;
        std::println("  Occurrence lookup (raw):  {} queries in {:.1f}ms ({:.0f} q/ms, {} hits)",
                     queries,
                     elapsed,
                     queries / elapsed,
                     hit_count);
    }

    // Relation lookup by symbol hash (raw, no position conversion).
    {
        std::size_t hit_count = 0;
        Timer t;
        constexpr int rounds = 10;
        for(int r = 0; r < rounds; r++) {
            for(auto& [path, shard]: merged) {
                for(auto hash: sample_hashes) {
                    shard.lookup(hash, RelationKind::Definition, [&](const index::Relation&) {
                        hit_count++;
                        return true;
                    });
                }
            }
        }
        auto elapsed = t.ms();
        auto queries = merged.size() * sample_hashes.size() * rounds;
        std::println("  Relation lookup (raw):    {} queries in {:.1f}ms ({:.0f} q/ms, {} hits)",
                     queries,
                     elapsed,
                     queries / elapsed,
                     hit_count);
    }

    // PositionMapper construction benchmark.
    {
        using kota::ipc::lsp::PositionMapper;
        using kota::ipc::lsp::PositionEncoding;

        std::size_t mapper_count = 0;
        std::vector<double> construct_times;

        for(auto& [path, shard]: merged) {
            auto content = shard.content();
            if(content.empty())
                continue;
            Timer t;
            PositionMapper mapper(content, PositionEncoding::UTF16);
            construct_times.push_back(t.ms());
            mapper_count++;
        }

        std::ranges::sort(construct_times);
        if(!construct_times.empty()) {
            auto sum = std::accumulate(construct_times.begin(), construct_times.end(), 0.0);
            auto p = [&](double pct) {
                return construct_times[static_cast<std::size_t>(construct_times.size() * pct)];
            };
            std::println("\n  PositionMapper construction ({} shards):", mapper_count);
            std::println("    Total: {:.1f}ms", sum);
            std::println("    p50={:.3f}ms  p90={:.3f}ms  p99={:.3f}ms  max={:.3f}ms",
                         p(0.5),
                         p(0.9),
                         p(0.99),
                         construct_times.back());
        }
    }

    // Relation lookup with full position conversion (simulates real LSP query).
    {
        using kota::ipc::lsp::PositionMapper;
        using kota::ipc::lsp::PositionEncoding;

        // Pre-build mappers for all shards.
        llvm::DenseMap<std::uint32_t, PositionMapper> mappers;
        for(auto& [path_id, shard]: merged) {
            auto content = shard.content();
            if(!content.empty())
                mappers.try_emplace(path_id, content, PositionEncoding::UTF16);
        }

        std::size_t hit_count = 0;
        std::size_t convert_count = 0;
        Timer t;
        constexpr int rounds = 10;
        for(int r = 0; r < rounds; r++) {
            for(auto& [path_id, shard]: merged) {
                auto mapper_it = mappers.find(path_id);
                if(mapper_it == mappers.end())
                    continue;
                auto& mapper = mapper_it->second;
                for(auto hash: sample_hashes) {
                    shard.lookup(hash, RelationKind::Definition, [&](const index::Relation& rel) {
                        auto start = mapper.to_position(rel.range.begin);
                        auto end = mapper.to_position(rel.range.end);
                        if(start && end)
                            convert_count++;
                        hit_count++;
                        return true;
                    });
                }
            }
        }
        auto elapsed = t.ms();
        auto queries = merged.size() * sample_hashes.size() * rounds;
        std::println("\n  Relation lookup + to_position:");
        std::println("    {} queries in {:.1f}ms ({:.0f} q/ms, {} hits, {} converted)",
                     queries,
                     elapsed,
                     queries / elapsed,
                     hit_count,
                     convert_count);
    }

    // Phase 4: Realistic "find references" for specific high-frequency symbols.
    std::println("=== Phase 4: Realistic Find-References ===");
    {
        using kota::ipc::lsp::PositionMapper;
        using kota::ipc::lsp::PositionEncoding;

        // Use the properly merged ProjectIndex symbol table.
        auto& symbol_table = project_index.symbols;

        // Find symbols by name to query. Collect ALL hashes whose name contains the target.
        llvm::StringRef targets[] = {"vector",
                                     "StringRef",
                                     "SourceLocation",
                                     "DenseMap",
                                     "SmallVector"};
        for(auto target_name: targets) {
            llvm::SmallVector<index::SymbolHash> matching_hashes;
            std::size_t ref_file_count = 0;
            for(auto& [hash, sym]: symbol_table) {
                if(sym.name == target_name) {
                    matching_hashes.push_back(hash);
                    ref_file_count += sym.reference_files.cardinality();
                }
            }

            if(matching_hashes.empty()) {
                std::println("  {}: not found in symbol table", target_name);
                continue;
            }

            // Query all shards for this symbol's references (simulating real find-references).
            std::size_t total_refs = 0;
            std::size_t shards_queried = 0;
            RelationKind kinds[] = {
                RelationKind::Reference,
                RelationKind::Definition,
                RelationKind::Declaration,
            };
            Timer t;
            for(auto& [path, shard]: merged) {
                bool found = false;
                for(auto target_hash: matching_hashes) {
                    for(auto kind: kinds) {
                        shard.lookup(target_hash, kind, [&](const index::Relation&) {
                            total_refs++;
                            found = true;
                            return true;
                        });
                    }
                }
                if(found)
                    shards_queried++;
            }
            auto elapsed = t.ms();

            std::println(
                "  {} ({} hashes, {} ref_files): {} refs across {} shards, " "scanned {} shards in {:.2f}ms",
                target_name,
                matching_hashes.size(),
                ref_file_count,
                total_refs,
                shards_queried,
                merged.size(),
                elapsed);

            // Now with reference_files bitmap to skip irrelevant shards.
            // We don't have path_id mapping here, so just measure the full scan above
            // vs a targeted scan using reference_files count as an estimate.
        }
    }

    std::println("");
    return 0;
}
