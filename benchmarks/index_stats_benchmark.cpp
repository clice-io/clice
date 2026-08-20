/// In-process measurement probe for the on-disk index redesign. Compiles
/// every TU in a compilation database exactly like the background-index
/// worker does (full parse without PCH + envelope build), measures the
/// index the way production consumes it, then walks the resulting structures
/// and accumulates the distributions the new "merged blob" format needs to
/// pick its column tiers.
///
/// Compiles run on a few worker threads, each accumulating into its own
/// Stats; the only shared state is a per-(path, variant-hash) registry
/// deciding which thread walks a distinct variant's rows — touched once per
/// file section, never per row — so "per distinct variant" populations are not
/// double-counted across threads. Accumulators merge after join.
///
/// This is measurement scratch code: it favours being obvious over being
/// fast or general.
///
/// Usage:
///   index_stats_benchmark [OPTIONS] <compile_commands.json>
///
/// Example:
///   ./build/RelWithDebInfo/bin/index_stats_benchmark \
///       build/RelWithDebInfo/compile_commands.json

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <print>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/format.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

using namespace clice;

namespace {

struct BenchmarkOptions {
    DecoKV(names = {"--log-level"}; help = "Log level: trace, debug, info, warn, error, off";
           required = false;)
    <std::string> log_level = "off";

    DecoKV(names = {"--filter"}; help = "Only process files whose path contains this substring";
           required = false;)
    <std::string> filter;

    DecoKV(names = {"--limit"}; help = "Process only the N largest files by source size (0 = all)";
           required = false;)
    <int> limit = 0;

    DecoKV(names = {"--threads"}; help = "Worker threads (1-4)"; required = false;)
    <int> threads = 4;

    DecoKV(names = {"--out-dir"}; help = "Directory for stats.json and REPORT.md";
           required = false;)
    <std::string> out_dir = "temp/index-stats";

    DecoFlag(names = {"-h", "--help"}; help = "Show help message"; required = false;)
    help;

    DecoInput(meta_var = "CDB"; help = "Path to compile_commands.json"; required = false;)
    <std::string> cdb_path;
};

constexpr std::array<llvm::StringRef, 16> relation_names = {"Invalid",
                                                            "Declaration",
                                                            "Definition",
                                                            "Reference",
                                                            "WeakReference",
                                                            "Read",
                                                            "Write",
                                                            "Interface",
                                                            "Implementation",
                                                            "TypeDef",
                                                            "Base",
                                                            "Derived",
                                                            "Constructor",
                                                            "Destructor",
                                                            "Caller",
                                                            "Callee"};

/// Percentiles over a raw sample, nearest-rank (matches bench::percentile and
/// the TS harness). Finalize before querying.
struct Dist {
    std::vector<std::uint64_t> samples;

    void add(std::uint64_t v) {
        samples.push_back(v);
    }

    void merge(const Dist& other) {
        samples.insert(samples.end(), other.samples.begin(), other.samples.end());
    }

    void finalize() {
        std::ranges::sort(samples);
    }

    std::uint64_t pct(double p) const {
        if(samples.empty()) {
            return 0;
        }
        auto index = static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
        return samples[index];
    }

    std::uint64_t max() const {
        return samples.empty() ? 0 : samples.back();
    }
};

/// A bucketed distribution for high-volume per-row samples (token lengths,
/// begin deltas), where holding one entry per row would be wasteful. Values
/// at or above `cap` fold into the last bucket but are counted in `over_cap`
/// and the exact `max_value` is kept.
template <std::size_t Cap>
struct Hist {
    std::array<std::uint64_t, Cap> buckets{};
    std::uint64_t total = 0;
    std::uint64_t over_cap = 0;
    std::uint64_t max_value = 0;

    void add(std::uint64_t v) {
        buckets[std::min<std::uint64_t>(v, Cap - 1)] += 1;
        total += 1;
        if(v >= Cap) {
            over_cap += 1;
        }
        if(v > max_value) {
            max_value = v;
        }
    }

    void merge(const Hist& other) {
        for(std::size_t i = 0; i < Cap; i += 1) {
            buckets[i] += other.buckets[i];
        }
        total += other.total;
        over_cap += other.over_cap;
        max_value = std::max(max_value, other.max_value);
    }

    std::uint64_t pct(double p) const {
        if(total == 0) {
            return 0;
        }
        auto rank = static_cast<std::uint64_t>(p * static_cast<double>(total - 1));
        std::uint64_t cum = 0;
        for(std::size_t i = 0; i < Cap; i += 1) {
            cum += buckets[i];
            if(cum > rank) {
                return i;
            }
        }
        return Cap - 1;
    }

    std::uint64_t count_ge(std::uint64_t threshold) const {
        std::uint64_t count = 0;
        for(std::size_t i = threshold; i < Cap; i += 1) {
            count += buckets[i];
        }
        return count;
    }
};

/// Per source-file aggregates, keyed by path string and folded across every
/// TU that touched the file.
struct PathAgg {
    /// Distinct section blob hashes this thread claimed for this path;
    /// claims are globally unique, so the merged union is the file's M.
    std::set<std::uint64_t> variants;
    /// Number of TU contributions (one section per TU per path) — N.
    std::uint32_t contributions = 0;

    bool size_probed = false;
    bool size_known = false;
    std::uint64_t file_size = 0;

    /// Distinct symbols (occurrence targets, relation keys and symbol
    /// payloads, mirroring the shard writer's referenced set) over all
    /// variants — S, the blob-local symbol id space.
    llvm::DenseSet<index::SymbolHash> symbols;

    std::uint64_t max_range_end = 0;

    /// Row counts summed over distinct variants (what the merged blob stores).
    std::uint64_t occ_m = 0, rel_m = 0, decldef_m = 0;
    /// Row counts summed over all contributions (no dedup).
    std::uint64_t occ_n = 0, rel_n = 0, decldef_n = 0;
};

/// Distinct outgoing include-list shapes seen for one (parent path, content
/// hash) key, plus how many TUs entered it.
struct DirAgg {
    llvm::DenseSet<std::uint64_t> shapes;
    std::uint32_t contributions = 0;
};

/// Arbitrates which thread accumulates a distinct (path, variant) exactly
/// once. Touched once per section per TU — a coarse coordination point,
/// not the per-row hot path.
struct VariantRegistry {
    std::mutex mutex;
    std::set<std::pair<std::string, std::uint64_t>> seen;

    bool try_claim(llvm::StringRef path, std::uint64_t hash) {
        std::scoped_lock lock(mutex);
        return seen.insert({path.str(), hash}).second;
    }
};

struct Stats {
    VariantRegistry* registry = nullptr;

    std::unordered_map<std::string, PathAgg> paths;

    /// Globally deduped symbol scope, first-seen wins.
    llvm::DenseMap<index::SymbolHash, index::SymbolScope> scope_map;
    /// Scope counts summed over all TU symbol tables (per-TU population).
    std::array<std::uint64_t, 3> scope_n{};

    /// Relation kind counts over distinct variants.
    std::array<std::uint64_t, 16> kind_hist{};

    Dist occ_counts, rel_counts, s_variant, wire_sizes;
    Hist<4096> length_hist;
    Hist<8192> delta_hist;

    llvm::StringMap<DirAgg> directive;

    std::uint64_t skipped_missing = 0;
    std::uint64_t skipped_compile = 0;
    std::uint64_t indexed = 0;
    std::uint64_t had_diagnostics = 0;

    /// One file's rows decoded back out of its envelope section.
    struct DecodedRows {
        std::vector<index::Occurrence> occurrences;
        llvm::DenseMap<index::SymbolHash, std::vector<index::Relation>> relations{};
    };

    /// `hash` is the section's blob hash — the variant's byte identity.
    void add_file_index(const DecodedRows& fi, llvm::StringRef path, std::uint64_t hash) {
        if(fi.occurrences.empty() && fi.relations.empty()) {
            return;
        }

        auto& agg = paths[path.str()];
        if(!agg.size_probed) {
            agg.size_probed = true;
            std::uint64_t size = 0;
            if(!llvm::sys::fs::file_size(path, size)) {
                agg.file_size = size;
                agg.size_known = true;
            }
        }
        agg.contributions += 1;

        bool inserted = registry->try_claim(path, hash);
        if(inserted) {
            agg.variants.insert(hash);
        }

        std::uint64_t occ = fi.occurrences.size();
        std::uint64_t rel = 0, decldef = 0;
        for(auto& [symbol, relations]: fi.relations) {
            rel += relations.size();
            for(auto& r: relations) {
                auto k = static_cast<std::uint32_t>(r.kind);
                if(r.kind == RelationKind::Declaration || r.kind == RelationKind::Definition) {
                    decldef += 1;
                }
                if(inserted && k < kind_hist.size()) {
                    kind_hist[k] += 1;
                }
            }
        }

        agg.occ_n += occ;
        agg.rel_n += rel;
        agg.decldef_n += decldef;

        if(!inserted) {
            return;
        }

        agg.occ_m += occ;
        agg.rel_m += rel;
        agg.decldef_m += decldef;
        occ_counts.add(occ);
        rel_counts.add(rel);

        llvm::DenseSet<index::SymbolHash> variant_symbols;
        for(auto& o: fi.occurrences) {
            variant_symbols.insert(o.target);
        }
        for(auto& [symbol, relations]: fi.relations) {
            variant_symbols.insert(symbol);
            // Non-decl/def payloads are symbol hashes the shard's table
            // must also cover (decl/def payloads are definition ranges).
            for(auto& r: relations) {
                if(r.target_symbol != 0 && !RelationKind(r.kind).isDeclOrDef()) {
                    variant_symbols.insert(r.target_symbol);
                }
            }
        }
        s_variant.add(variant_symbols.size());
        for(auto s: variant_symbols) {
            agg.symbols.insert(s);
        }

        bool first = true;
        std::uint32_t prev_begin = 0;
        for(auto& o: fi.occurrences) {
            length_hist.add(o.range.length());
            if(o.range.end > agg.max_range_end) {
                agg.max_range_end = o.range.end;
            }
            if(!first) {
                delta_hist.add(o.range.begin >= prev_begin ? o.range.begin - prev_begin : 0);
            }
            prev_begin = o.range.begin;
            first = false;
        }
    }

    void add_directives(const index::TUIndex& tu) {
        if(tu.path_count() == 0) {
            return;
        }
        auto root_path_id = tu.path_count() - 1;

        // Outgoing edges keyed by parent location index (-1 = TU root).
        llvm::DenseMap<std::int64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> outgoing;
        for(std::uint32_t i = 0; i < tu.location_count(); i += 1) {
            auto loc = tu.location(i);
            std::int64_t parent = loc.include == static_cast<std::uint32_t>(-1)
                                      ? -1
                                      : static_cast<std::int64_t>(loc.include);
            outgoing[parent].emplace_back(loc.line, loc.path_id);
        }

        llvm::StringMap<llvm::DenseSet<std::uint64_t>> local;
        for(auto& [parent, list]: outgoing) {
            std::uint32_t parent_path_id =
                parent < 0 ? root_path_id : tu.location(static_cast<std::uint32_t>(parent)).path_id;
            if(parent_path_id >= tu.path_count()) {
                continue;
            }
            std::uint64_t parent_hash = tu.path_hash(parent_path_id);

            std::ranges::sort(list, [&](auto& a, auto& b) {
                if(a.first != b.first) {
                    return a.first < b.first;
                }
                return tu.path(a.second) < tu.path(b.second);
            });

            std::string shape;
            for(auto& [line, child_path_id]: list) {
                shape += std::format("{},{}\n",
                                     line,
                                     child_path_id < tu.path_count() ? tu.path(child_path_id)
                                                                     : llvm::StringRef());
            }

            auto key = std::format("{}#{:016x}", tu.path(parent_path_id), parent_hash);
            local[key].insert(llvm::xxh3_64bits(shape));
        }

        for(auto& entry: local) {
            auto& agg = directive[entry.getKey()];
            for(auto shape: entry.getValue()) {
                agg.shapes.insert(shape);
            }
            agg.contributions += 1;
        }
    }

    void add_tu(const index::TUIndex& tu) {
        indexed += 1;

        tu.iterate_symbols(
            [&](index::SymbolHash hash, const index::SymbolIdentity& symbol, llvm::StringRef) {
                auto scope = static_cast<std::uint8_t>(symbol.scope);
                if(scope < scope_n.size()) {
                    scope_n[scope] += 1;
                }
                scope_map.try_emplace(hash, symbol.scope);
                return true;
            });

        for(std::uint32_t i = 0; i < tu.section_count(); i += 1) {
            const auto& shard = tu.shard_of(tu.section_path(i));
            DecodedRows rows;
            shard.for_each_occurrence([&](const index::Occurrence& occurrence) {
                rows.occurrences.push_back(occurrence);
                return true;
            });
            shard.for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
                rows.relations[hash].push_back(relation);
                return true;
            });
            add_file_index(rows, tu.path(tu.section_path(i)), tu.section_hash(i));
        }

        add_directives(tu);
    }

    void merge(Stats& other) {
        for(auto& [path, o]: other.paths) {
            auto& a = paths[path];
            a.variants.merge(o.variants);
            a.contributions += o.contributions;
            if(!a.size_probed && o.size_probed) {
                a.size_probed = true;
                a.size_known = o.size_known;
                a.file_size = o.file_size;
            }
            for(auto s: o.symbols) {
                a.symbols.insert(s);
            }
            a.max_range_end = std::max(a.max_range_end, o.max_range_end);
            a.occ_m += o.occ_m;
            a.rel_m += o.rel_m;
            a.decldef_m += o.decldef_m;
            a.occ_n += o.occ_n;
            a.rel_n += o.rel_n;
            a.decldef_n += o.decldef_n;
        }

        for(auto& [hash, scope]: other.scope_map) {
            scope_map.try_emplace(hash, scope);
        }
        for(std::size_t i = 0; i < scope_n.size(); i += 1) {
            scope_n[i] += other.scope_n[i];
        }
        for(std::size_t i = 0; i < kind_hist.size(); i += 1) {
            kind_hist[i] += other.kind_hist[i];
        }

        occ_counts.merge(other.occ_counts);
        rel_counts.merge(other.rel_counts);
        s_variant.merge(other.s_variant);
        wire_sizes.merge(other.wire_sizes);
        length_hist.merge(other.length_hist);
        delta_hist.merge(other.delta_hist);

        for(auto& entry: other.directive) {
            auto& agg = directive[entry.getKey()];
            for(auto shape: entry.getValue().shapes) {
                agg.shapes.insert(shape);
            }
            agg.contributions += entry.getValue().contributions;
        }

        skipped_missing += other.skipped_missing;
        skipped_compile += other.skipped_compile;
        indexed += other.indexed;
        had_diagnostics += other.had_diagnostics;
    }
};

/// Per-row mask cost of the shard writer's tier ladder (tier_of in
/// src/index/shard.cpp): none, u32, u64, then roaring — a 4-byte offset
/// column entry plus the serialized bitmap, whose floor is 18 bytes (the
/// 8-byte portable header plus one single-value array container).
std::uint32_t mask_bytes(std::uint32_t m) {
    if(m <= 1) {
        return 0;
    }
    if(m <= 32) {
        return 4;
    }
    if(m <= 64) {
        return 8;
    }
    return 4 + 18;
}

CompilationParams make_params(const std::vector<const char*>& arguments,
                              llvm::StringRef file,
                              llvm::StringRef content) {
    CompilationParams params;
    params.kind = CompilationKind::Indexing;
    params.arguments = arguments;
    params.add_remapped_file(file, content);
    return params;
}

/// Everything derived from a finalized Stats, computed once and shared by the
/// stdout summary, REPORT.md and stats.json.
struct Report {
    // M / N
    std::array<std::uint64_t, 6> m_buckets{};  // 1 / 2-8 / 9-16 / 17-32 / 33-64 / >64
    Dist m_dist, n_dist;
    std::uint64_t sum_m = 0, sum_n = 0;
    double variant_hit_rate = 0;

    // S
    Dist s_dist;
    std::uint64_t s_over_65535 = 0;
    std::uint64_t path_count = 0;

    // occurrences / relations per variant
    Dist occ_dist, rel_dist, s_variant_dist;

    // token length
    std::uint64_t length_p50 = 0, length_p99 = 0, length_max = 0, length_total = 0;
    std::uint64_t length_over_255 = 0, length_over_cap = 0;

    // begin deltas
    std::uint64_t delta_p50 = 0, delta_p90 = 0, delta_over_cap = 0, delta_total = 0;

    // max range end
    Dist max_end_dist;
    std::uint64_t max_end_under_64k = 0;

    // u16 offset coverage
    std::uint64_t files_under_64k = 0, files_sized = 0;
    std::uint64_t occ_total = 0, occ_under_64k = 0;

    // relations
    std::array<std::uint64_t, 16> kind_hist{};
    std::uint64_t rel_total = 0, range_payload = 0;

    // scope
    std::array<std::uint64_t, 3> scope_dedup{};
    std::array<std::uint64_t, 3> scope_n{};

    // directive sharing
    std::uint64_t dir_keys = 0, dir_identical = 0, dir_shapes_sum = 0;
    std::uint64_t dir_multi_keys = 0, dir_multi_identical = 0, dir_multi_shapes_sum = 0;

    // size accounting
    std::uint64_t today_m = 0, new_m = 0, today_n = 0, new_n = 0;
    std::uint64_t wire_total = 0;
    Dist wire_dist;
};

Report build_report(Stats& stats) {
    Report r;
    r.path_count = stats.paths.size();

    for(auto& [path, agg]: stats.paths) {
        auto m = static_cast<std::uint32_t>(agg.variants.size());
        r.m_dist.add(m);
        r.n_dist.add(agg.contributions);
        r.sum_m += m;
        r.sum_n += agg.contributions;

        std::size_t bucket = m <= 1 ? 0 : m <= 8 ? 1 : m <= 16 ? 2 : m <= 32 ? 3 : m <= 64 ? 4 : 5;
        r.m_buckets[bucket] += 1;

        auto s = static_cast<std::uint64_t>(agg.symbols.size());
        r.s_dist.add(s);
        if(s > 65535) {
            r.s_over_65535 += 1;
        }

        r.max_end_dist.add(agg.max_range_end);
        if(agg.max_range_end < 65536) {
            r.max_end_under_64k += 1;
        }

        bool small = agg.size_known && agg.file_size < 65536;
        if(agg.size_known) {
            r.files_sized += 1;
            if(small) {
                r.files_under_64k += 1;
            }
        }
        r.occ_total += agg.occ_m;
        if(small) {
            r.occ_under_64k += agg.occ_m;
        }

        // The shard writer emits fixed u32 begin columns at every file size;
        // the u16 coverage stats above measure what a size-tiered offset
        // column would win, not what the implemented format spends.
        std::uint32_t off = 4;
        std::uint32_t sid = s <= 65535 ? 2 : 4;
        std::uint32_t mask = mask_bytes(m);

        r.today_m += agg.occ_m * 16 + agg.rel_m * 24;
        r.new_m += agg.occ_m * (off + 1 + sid + mask);
        r.new_m += agg.rel_m * (1 + off + 1 + mask) + agg.decldef_m * 8 +
                   (agg.rel_m - agg.decldef_m) * sid;

        r.today_n += agg.occ_n * 16 + agg.rel_n * 24;
        r.new_n += agg.occ_n * (off + 1 + sid + mask);
        r.new_n += agg.rel_n * (1 + off + 1 + mask) + agg.decldef_n * 8 +
                   (agg.rel_n - agg.decldef_n) * sid;
    }
    r.variant_hit_rate = r.sum_n == 0 ? 0 : 1.0 - static_cast<double>(r.sum_m) / r.sum_n;

    r.m_dist.finalize();
    r.n_dist.finalize();
    r.s_dist.finalize();
    r.max_end_dist.finalize();

    stats.occ_counts.finalize();
    stats.rel_counts.finalize();
    stats.s_variant.finalize();
    stats.wire_sizes.finalize();
    r.occ_dist = stats.occ_counts;
    r.rel_dist = stats.rel_counts;
    r.s_variant_dist = stats.s_variant;
    r.wire_dist = stats.wire_sizes;

    r.length_total = stats.length_hist.total;
    r.length_p50 = stats.length_hist.pct(0.5);
    r.length_p99 = stats.length_hist.pct(0.99);
    r.length_max = stats.length_hist.max_value;
    r.length_over_255 = stats.length_hist.count_ge(256);
    r.length_over_cap = stats.length_hist.over_cap;

    r.delta_total = stats.delta_hist.total;
    r.delta_p50 = stats.delta_hist.pct(0.5);
    r.delta_p90 = stats.delta_hist.pct(0.9);
    r.delta_over_cap = stats.delta_hist.over_cap;

    r.kind_hist = stats.kind_hist;
    for(auto c: stats.kind_hist) {
        r.rel_total += c;
    }
    r.range_payload =
        stats.kind_hist[RelationKind::Declaration] + stats.kind_hist[RelationKind::Definition];

    for(auto& [hash, scope]: stats.scope_map) {
        auto s = static_cast<std::uint8_t>(scope);
        if(s < r.scope_dedup.size()) {
            r.scope_dedup[s] += 1;
        }
    }
    r.scope_n = stats.scope_n;

    for(auto& entry: stats.directive) {
        auto shapes = entry.getValue().shapes.size();
        r.dir_keys += 1;
        r.dir_shapes_sum += shapes;
        if(shapes == 1) {
            r.dir_identical += 1;
        }
        if(entry.getValue().contributions >= 2) {
            r.dir_multi_keys += 1;
            r.dir_multi_shapes_sum += shapes;
            if(shapes == 1) {
                r.dir_multi_identical += 1;
            }
        }
    }

    for(auto w: stats.wire_sizes.samples) {
        r.wire_total += w;
    }

    return r;
}

std::string format_report_md(const Stats& stats, const Report& r, llvm::StringRef cdb) {
    auto frac = [](std::uint64_t num, std::uint64_t den) {
        return den == 0 ? 0.0 : 100.0 * static_cast<double>(num) / static_cast<double>(den);
    };
    std::string o;
    auto line = [&](std::string s) {
        o += s;
        o += '\n';
    };

    line("# Index format distribution report");
    line("");
    line(std::format("- CDB: `{}`", cdb));
    line(
        std::format("- TUs indexed: {} (skipped: {} missing file, {} did not compile; {} indexed "
                    "with diagnostics)",
                    stats.indexed,
                    stats.skipped_missing,
                    stats.skipped_compile,
                    stats.had_diagnostics));
    line(std::format("- Distinct source files (paths): {}", r.path_count));
    line("");
    line(
        "> Caveat: this CDB is single-config, so **M is a lower bound** — a "
        "multi-config project spreads over more preprocessing variants per file.");
    line("");

    line("## 1. Variants per file (M) and TU count (N)");
    line("");
    line("| M bucket | files |");
    line("|---|---|");
    constexpr static std::array<llvm::StringRef, 6> labels =
        {"1", "2-8", "9-16", "17-32", "33-64", ">64"};
    for(std::size_t i = 0; i < 6; i += 1) {
        line(std::format("| {} | {} |", labels[i], r.m_buckets[i]));
    }
    line("");
    line("| metric | p50 | p90 | p99 | max |");
    line("|---|---|---|---|---|");
    line(std::format("| M | {} | {} | {} | {} |",
                     r.m_dist.pct(0.5),
                     r.m_dist.pct(0.9),
                     r.m_dist.pct(0.99),
                     r.m_dist.max()));
    line(std::format("| N | {} | {} | {} | {} |",
                     r.n_dist.pct(0.5),
                     r.n_dist.pct(0.9),
                     r.n_dist.pct(0.99),
                     r.n_dist.max()));
    line("");
    line(std::format("- sum(M) = {}, sum(N) = {}", r.sum_m, r.sum_n));
    line(std::format("- variant hit rate (1 - sum(M)/sum(N)) = {:.2f}%",
                     100.0 * r.variant_hit_rate));
    line("");

    line("## 2. Distinct symbols per file (S)");
    line("");
    line("| population | p50 | p99 | max |");
    line("|---|---|---|---|");
    line(std::format("| per path (all variants) | {} | {} | {} |",
                     r.s_dist.pct(0.5),
                     r.s_dist.pct(0.99),
                     r.s_dist.max()));
    line(std::format("| per single variant | {} | {} | {} |",
                     r.s_variant_dist.pct(0.5),
                     r.s_variant_dist.pct(0.99),
                     r.s_variant_dist.max()));
    line("");
    line(std::format("- files with S > 65535: {} / {} ({:.3f}%)  →  **u16 symbol id {}**",
                     r.s_over_65535,
                     r.path_count,
                     frac(r.s_over_65535, r.path_count),
                     r.s_over_65535 == 0 ? "safe" : "NOT safe"));
    line("");

    line("## 3. Occurrences");
    line("");
    line("| metric | p50 | p90 | p99 | max |");
    line("|---|---|---|---|---|");
    line(std::format("| occurrences / variant | {} | {} | {} | {} |",
                     r.occ_dist.pct(0.5),
                     r.occ_dist.pct(0.9),
                     r.occ_dist.pct(0.99),
                     r.occ_dist.max()));
    line(std::format("| token length | {} | - | {} | {} |",
                     r.length_p50,
                     r.length_p99,
                     r.length_max));
    line(std::format("| begin delta | {} | {} | - | - |", r.delta_p50, r.delta_p90));
    line(std::format("| max range.end / file | {} | {} | {} | {} |",
                     r.max_end_dist.pct(0.5),
                     r.max_end_dist.pct(0.9),
                     r.max_end_dist.pct(0.99),
                     r.max_end_dist.max()));
    line("");
    line(std::format("- token length > 255: {} / {} ({:.3f}%)  →  **u8 length {}**",
                     r.length_over_255,
                     r.length_total,
                     frac(r.length_over_255, r.length_total),
                     r.length_over_255 == 0 ? "safe" : "NOT safe (some tokens exceed 255)"));
    line(std::format("- token length >= 4096 (histogram overflow): {}", r.length_over_cap));
    line(std::format("- begin delta >= 8192 (histogram overflow): {}", r.delta_over_cap));
    line(std::format("- files with max range.end < 64KB: {} / {} ({:.2f}%)",
                     r.max_end_under_64k,
                     r.path_count,
                     frac(r.max_end_under_64k, r.path_count)));
    line("");

    line("## 4. u16 offset coverage");
    line("");
    line(std::format("- files < 64KB on disk: {} / {} sized ({:.2f}%)",
                     r.files_under_64k,
                     r.files_sized,
                     frac(r.files_under_64k, r.files_sized)));
    line(std::format("- occurrences residing in files < 64KB: {} / {} ({:.2f}%)",
                     r.occ_under_64k,
                     r.occ_total,
                     frac(r.occ_under_64k, r.occ_total)));
    line("");

    line("## 5. Relations");
    line("");
    line("| kind | count |");
    line("|---|---|");
    for(std::size_t i = 0; i < 16; i += 1) {
        if(r.kind_hist[i] > 0) {
            line(std::format("| {} | {} |", relation_names[i], r.kind_hist[i]));
        }
    }
    line("");
    line("| metric | p50 | p90 | p99 | max |");
    line("|---|---|---|---|---|");
    line(std::format("| relations / variant | {} | {} | {} | {} |",
                     r.rel_dist.pct(0.5),
                     r.rel_dist.pct(0.9),
                     r.rel_dist.pct(0.99),
                     r.rel_dist.max()));
    line("");
    line(
        "- range-payload kinds (carry a definition range in the target slot "
        "instead of a symbol hash): **Declaration, Definition**");
    line(std::format("- range-payload relations: {} / {} ({:.2f}%)",
                     r.range_payload,
                     r.rel_total,
                     frac(r.range_payload, r.rel_total)));
    line("");

    line("## 6. Symbol scope split");
    line("");
    line("| scope | deduped by hash | per-TU sum |");
    line("|---|---|---|");
    constexpr static std::array<llvm::StringRef, 3> scope_labels = {"External",
                                                                    "TULocal",
                                                                    "FileLocal"};
    for(std::size_t i = 0; i < 3; i += 1) {
        line(std::format("| {} | {} | {} |", scope_labels[i], r.scope_dedup[i], r.scope_n[i]));
    }
    std::uint64_t scope_total = r.scope_dedup[0] + r.scope_dedup[1] + r.scope_dedup[2];
    line("");
    line(std::format("- deduped total {} → External {:.1f}%, TULocal {:.1f}%, FileLocal {:.1f}%",
                     scope_total,
                     frac(r.scope_dedup[0], scope_total),
                     frac(r.scope_dedup[1], scope_total),
                     frac(r.scope_dedup[2], scope_total)));
    line("");

    line("## 7. Include directive sharing");
    line("");
    line(std::format("- (parent path, content hash) keys: {}", r.dir_keys));
    line(std::format("- keys with one shape across all TUs: {} / {} ({:.2f}%)",
                     r.dir_identical,
                     r.dir_keys,
                     frac(r.dir_identical, r.dir_keys)));
    line(std::format("- avg distinct shapes per key: {:.4f}",
                     r.dir_keys == 0 ? 0.0 : static_cast<double>(r.dir_shapes_sum) / r.dir_keys));
    line(std::format("- keys entered by >= 2 TUs: {}", r.dir_multi_keys));
    line(std::format("- of those, identical in every TU: {} / {} ({:.2f}%); avg shapes {:.4f}",
                     r.dir_multi_identical,
                     r.dir_multi_keys,
                     frac(r.dir_multi_identical, r.dir_multi_keys),
                     r.dir_multi_keys == 0
                         ? 0.0
                         : static_cast<double>(r.dir_multi_shapes_sum) / r.dir_multi_keys));
    line("");

    line("## 8. Size accounting");
    line("");
    line("| population | today (occ*16 + rel*24) | new-format estimate | ratio |");
    line("|---|---|---|---|");
    line(std::format("| distinct variants (M, merged blob) | {} | {} | {:.3f} |",
                     r.today_m,
                     r.new_m,
                     r.today_m == 0 ? 0.0 : static_cast<double>(r.new_m) / r.today_m));
    line(std::format("| all contributions (N) | {} | {} | {:.3f} |",
                     r.today_n,
                     r.new_n,
                     r.today_n == 0 ? 0.0 : static_cast<double>(r.new_n) / r.today_n));
    line("");
    line(std::format("- total serialized TUIndex wire size (all TUs): {}", r.wire_total));
    line(std::format("- per-TU wire size: p50 {}, p90 {}, max {}",
                     r.wire_dist.pct(0.5),
                     r.wire_dist.pct(0.9),
                     r.wire_dist.max()));
    line("");
    line(
        "> New-format estimate assigns no cross-variant row-dedup credit, so "
        "for M>1 files it is an upper bound.");
    line("");

    line("## Answers");
    line("");
    line(std::format("- (a) mask tiers given observed M (max {}): {}",
                     r.m_dist.max(),
                     r.m_dist.max() <= 1
                         ? "every file is single-variant here → 0-bit masks suffice; the "
                           "u32/u64/roaring tier ladder is exercised only by multi-config projects"
                         : "M spreads beyond 1 → tiered masks pay off"));
    line(std::format("- (b) u16 symbol id: {} ({} files exceed 65535 symbols)",
                     r.s_over_65535 == 0 ? "SAFE" : "UNSAFE",
                     r.s_over_65535));
    line(std::format("- (c) u8 token length: {} ({:.3f}% of occurrences exceed 255)",
                     r.length_over_255 == 0 ? "SAFE" : "needs escape/overflow path",
                     frac(r.length_over_255, r.length_total)));
    line(
        std::format("- (d) u16 offset coverage: {:.2f}% of sized files < 64KB, "
                    "{:.2f}% of occurrences in files < 64KB",
                    frac(r.files_under_64k, r.files_sized),
                    frac(r.occ_under_64k, r.occ_total)));
    line(std::format("- (e) range-payload relation fraction: {:.2f}%",
                     frac(r.range_payload, r.rel_total)));
    line(
        std::format("- (f) directive sharing: {:.2f}% of all keys single-shape, "
                    "{:.2f}% of multi-TU keys single-shape",
                    frac(r.dir_identical, r.dir_keys),
                    frac(r.dir_multi_identical, r.dir_multi_keys)));
    line(
        std::format("- (g) size totals (merged M population): today {} bytes vs new-format "
                    "estimate {} bytes ({:.1f}% of today)",
                    r.today_m,
                    r.new_m,
                    r.today_m == 0 ? 0.0 : 100.0 * static_cast<double>(r.new_m) / r.today_m));
    line("");
    line(
        "Optional merged-blob replay (exact today shard sizes): **skipped** — it "
        "needs the workspace path pool and per-shard disk content plumbing, out of "
        "scope for this scratch probe.");

    return o;
}

std::string format_stats_json(const Stats& stats, const Report& r, llvm::StringRef cdb) {
    std::string o = "{\n";
    auto kv = [&](llvm::StringRef k, auto v, bool comma = true) {
        o += std::format("  \"{}\": {}{}\n", k, v, comma ? "," : "");
    };

    // The only string value in the output; a Windows path would otherwise
    // break the JSON.
    std::string escaped_cdb;
    for(char c: cdb) {
        if(c == '"' || c == '\\') {
            escaped_cdb += '\\';
        }
        escaped_cdb += c;
    }
    o += std::format("  \"cdb\": \"{}\",\n", escaped_cdb);
    kv("tus_indexed", stats.indexed);
    kv("skipped_missing", stats.skipped_missing);
    kv("skipped_compile", stats.skipped_compile);
    kv("indexed_with_diagnostics", stats.had_diagnostics);
    kv("paths", r.path_count);

    o += "  \"m_buckets\": {";
    constexpr static std::array<llvm::StringRef, 6> labels =
        {"1", "2_8", "9_16", "17_32", "33_64", "gt64"};
    for(std::size_t i = 0; i < 6; i += 1) {
        o += std::format("\"{}\": {}{}", labels[i], r.m_buckets[i], i + 1 < 6 ? ", " : "");
    }
    o += "},\n";

    kv("m_p50", r.m_dist.pct(0.5));
    kv("m_p90", r.m_dist.pct(0.9));
    kv("m_p99", r.m_dist.pct(0.99));
    kv("m_max", r.m_dist.max());
    kv("n_p50", r.n_dist.pct(0.5));
    kv("n_p90", r.n_dist.pct(0.9));
    kv("n_p99", r.n_dist.pct(0.99));
    kv("n_max", r.n_dist.max());
    kv("sum_m", r.sum_m);
    kv("sum_n", r.sum_n);
    kv("variant_hit_rate", std::format("{:.6f}", r.variant_hit_rate));

    kv("s_p50", r.s_dist.pct(0.5));
    kv("s_p99", r.s_dist.pct(0.99));
    kv("s_max", r.s_dist.max());
    kv("s_variant_p50", r.s_variant_dist.pct(0.5));
    kv("s_variant_p99", r.s_variant_dist.pct(0.99));
    kv("s_variant_max", r.s_variant_dist.max());
    kv("s_over_65535", r.s_over_65535);

    kv("occ_per_variant_p50", r.occ_dist.pct(0.5));
    kv("occ_per_variant_p99", r.occ_dist.pct(0.99));
    kv("occ_per_variant_max", r.occ_dist.max());
    kv("length_p50", r.length_p50);
    kv("length_p99", r.length_p99);
    kv("length_max", r.length_max);
    kv("length_total", r.length_total);
    kv("length_over_255", r.length_over_255);
    kv("delta_p50", r.delta_p50);
    kv("delta_p90", r.delta_p90);
    kv("max_range_end_p99", r.max_end_dist.pct(0.99));
    kv("max_range_end_max", r.max_end_dist.max());
    kv("max_range_end_under_64k", r.max_end_under_64k);

    kv("files_under_64k", r.files_under_64k);
    kv("files_sized", r.files_sized);
    kv("occ_total", r.occ_total);
    kv("occ_under_64k", r.occ_under_64k);

    o += "  \"relation_kinds\": {";
    bool first = true;
    for(std::size_t i = 0; i < 16; i += 1) {
        if(r.kind_hist[i] > 0) {
            o += std::format("{}\"{}\": {}", first ? "" : ", ", relation_names[i], r.kind_hist[i]);
            first = false;
        }
    }
    o += "},\n";
    kv("rel_total", r.rel_total);
    kv("range_payload_relations", r.range_payload);
    kv("rel_per_variant_p50", r.rel_dist.pct(0.5));
    kv("rel_per_variant_p99", r.rel_dist.pct(0.99));
    kv("rel_per_variant_max", r.rel_dist.max());

    o += std::format(
        "  \"scope_dedup\": {{\"External\": {}, \"TULocal\": {}, \"FileLocal\": {}}},\n",
        r.scope_dedup[0],
        r.scope_dedup[1],
        r.scope_dedup[2]);
    o += std::format(
        "  \"scope_per_tu\": {{\"External\": {}, \"TULocal\": {}, \"FileLocal\": {}}},\n",
        r.scope_n[0],
        r.scope_n[1],
        r.scope_n[2]);

    kv("dir_keys", r.dir_keys);
    kv("dir_identical", r.dir_identical);
    kv("dir_shapes_sum", r.dir_shapes_sum);
    kv("dir_multi_keys", r.dir_multi_keys);
    kv("dir_multi_identical", r.dir_multi_identical);
    kv("dir_multi_shapes_sum", r.dir_multi_shapes_sum);

    kv("size_today_m", r.today_m);
    kv("size_new_m", r.new_m);
    kv("size_today_n", r.today_n);
    kv("size_new_n", r.new_n);
    kv("wire_total", r.wire_total);
    kv("wire_p50", r.wire_dist.pct(0.5));
    kv("wire_p90", r.wire_dist.pct(0.9));
    kv("wire_max", r.wire_dist.max(), false);

    o += "}\n";
    return o;
}

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto result = kota::deco::cli::parse<BenchmarkOptions>(args);
    if(!result.has_value()) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }
    auto& opts = result->options;

    if(opts.help.value_or(false) || !opts.cdb_path.has_value()) {
        std::ostringstream oss;
        kota::deco::cli::write_usage_for<BenchmarkOptions>(oss,
                                                           "index_stats_benchmark [OPTIONS] <cdb>");
        std::print("{}", oss.str());
        return opts.help.value_or(false) ? 0 : 1;
    }

    clice::logging::options.level = spdlog::level::from_str(*opts.log_level);
    clice::logging::stderr_logger("index_stats_benchmark", clice::logging::options);

    CompilationDatabase cdb;
    Toolchain toolchain;
    auto count = cdb.load(*opts.cdb_path);
    if(!count) {
        std::println(stderr, "Error: failed to load {}", *opts.cdb_path);
        return 1;
    }
    std::println("CDB loaded: {} entries", *count);

    // Distinct source paths: lookup() below returns every command of a
    // file at once, and the --limit selection sizes files, not entries.
    std::vector<llvm::StringRef> files;
    llvm::StringSet<> seen_files;
    for(auto& entry: cdb.get_entries()) {
        auto path = cdb.resolve_path(entry.file);
        if(opts.filter.has_value() && !path.contains(*opts.filter)) {
            continue;
        }
        if(!seen_files.insert(path).second) {
            continue;
        }
        files.push_back(path);
    }

    if(*opts.limit > 0 && files.size() > static_cast<std::size_t>(*opts.limit)) {
        std::vector<std::pair<std::uint64_t, llvm::StringRef>> sized;
        sized.reserve(files.size());
        for(auto path: files) {
            std::uint64_t size = 0;
            if(llvm::sys::fs::file_size(path, size)) {
                size = 0;
            }
            sized.emplace_back(size, path);
        }
        std::ranges::stable_sort(sized, std::greater{}, [](auto& pair) { return pair.first; });
        files.clear();
        for(auto& [size, path]: sized | std::views::take(*opts.limit)) {
            files.push_back(path);
        }
    }

    /// Command lookup and toolchain resolution stay on the main thread;
    /// workers receive ready-to-use argv (interned, pointer-stable).
    struct Job {
        llvm::StringRef file;
        std::vector<const char*> argv;
    };

    // One job per distinct command line: literal CDB duplicates would
    // recompile the same variant and inflate the N-side distributions,
    // while distinct commands for one file (per-config -D/-I/language
    // spreads) are exactly the population that creates preprocessing
    // variants and must all be compiled.
    std::vector<Job> jobs;
    jobs.reserve(files.size());
    llvm::DenseSet<std::uint64_t> seen_commands;
    for(auto file: files) {
        for(auto& command: cdb.lookup(file)) {
            toolchain.resolve_or_warn(command);
            auto argv = command.to_argv();
            std::string joined;
            for(auto* arg: argv) {
                joined += arg;
                joined += '\0';
            }
            if(seen_commands.insert(llvm::xxh3_64bits(joined)).second) {
                jobs.push_back({file, std::move(argv)});
            }
        }
    }

    auto thread_count = std::clamp(*opts.threads, 1, 4);
    std::println("Processing {} file(s) on {} thread(s)\n", jobs.size(), thread_count);

    VariantRegistry registry;
    std::vector<Stats> stats_by_thread(thread_count);
    for(auto& stats: stats_by_thread) {
        stats.registry = &registry;
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> done{0};
    auto worker = [&](Stats& stats) {
        while(true) {
            auto i = next.fetch_add(1);
            if(i >= jobs.size()) {
                break;
            }
            auto& job = jobs[i];

            auto finish = [&](llvm::StringRef verdict) {
                auto d = done.fetch_add(1) + 1;
                if(!verdict.empty()) {
                    std::println(stderr, "  [{}/{}] {} {}", d, jobs.size(), verdict, job.file);
                } else if(d % 10 == 0) {
                    std::println(stderr, "  [{}/{}] last {}", d, jobs.size(), job.file);
                }
            };

            auto content = fs::read(job.file);
            if(!content) {
                stats.skipped_missing += 1;
                finish("skip (unreadable)");
                continue;
            }

            auto params = make_params(job.argv, job.file, *content);
            auto unit = compile(params);
            if(!unit.completed()) {
                stats.skipped_compile += 1;
                finish("skip (did not compile)");
                continue;
            }
            if(auto errors = collect_errors(unit); !errors.empty()) {
                stats.had_diagnostics += 1;
            }

            auto envelope = index::build_tu_index(unit);
            stats.wire_sizes.add(envelope.size());

            stats.add_tu(index::TUIndex::from_bytes(envelope));
            finish("");
        }
    };

    std::vector<std::thread> threads;
    for(auto& stats: stats_by_thread) {
        threads.emplace_back(worker, std::ref(stats));
    }
    for(auto& thread: threads) {
        thread.join();
    }

    Stats stats = std::move(stats_by_thread[0]);
    for(std::size_t i = 1; i < stats_by_thread.size(); i += 1) {
        stats.merge(stats_by_thread[i]);
    }

    std::println(stderr, "");
    std::println("Indexed {} TU(s); skipped {} missing, {} non-compiling; {} had diagnostics",
                 stats.indexed,
                 stats.skipped_missing,
                 stats.skipped_compile,
                 stats.had_diagnostics);

    auto report = build_report(stats);
    auto md = format_report_md(stats, report, *opts.cdb_path);
    auto json = format_stats_json(stats, report, *opts.cdb_path);

    if(auto error = llvm::sys::fs::create_directories(*opts.out_dir)) {
        std::println(stderr, "Error: cannot create {}: {}", *opts.out_dir, error.message());
        return 1;
    }
    auto md_path = std::format("{}/REPORT.md", *opts.out_dir);
    auto json_path = std::format("{}/stats.json", *opts.out_dir);
    if(auto w = fs::write(md_path, md); !w) {
        std::println(stderr, "Error: cannot write {}: {}", md_path, w.error().message());
        return 1;
    }
    if(auto w = fs::write(json_path, json); !w) {
        std::println(stderr, "Error: cannot write {}: {}", json_path, w.error().message());
        return 1;
    }

    std::print("\n{}", md);
    std::println("Wrote {} and {}", md_path, json_path);
    return 0;
}
