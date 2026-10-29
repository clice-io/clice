module;

#include "support/prelude.h"

module clice;

import :command.file_kind;
import :feature.counterparts;
import :feature.query;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.symbol;
import :semantic.symbol;
import :syntax.dependency_graph;
import :syntax.scan;
import :vfs.file_table;
import :vfs.path;

namespace clice::feature {

namespace {

/// Which side of the split between declaring and defining a file is on.
enum class Side : std::uint8_t {
    /// Headers, and the module units other units import.
    Interface,
    /// Sources, module implementation units, and the template definitions
    /// a header pulls in (`.inl`, `.tpp`, ...).
    Implementation,
    /// `.inc` and `.def` files: pieces of their includers' text, with no
    /// counterpart of their own.
    Fragment,
};

Side classify(FileTable& files, const DependencyGraph& graph, Fid file) {
    llvm::StringRef path = files.resolve(file);
    auto extension = path::extension(path);
    if(extension == ".inc" || extension == ".def") {
        return Side::Fragment;
    }
    if(is_context_header_path(path)) {
        return Side::Implementation;
    }
    if(is_header_path(path) || !graph.module_of(file).empty()) {
        return Side::Interface;
    }
    return Side::Implementation;
}

constexpr unsigned shared_bit = 4;
constexpr unsigned name_bit = 2;
constexpr unsigned module_bit = 1;

/// The kinds of evidence a candidate has as bits, a stronger kind a higher
/// bit.
unsigned signals(const CounterpartCandidate& candidate) {
    return (candidate.overlap > 0 ? shared_bit : 0) | (candidate.same_name ? name_bit : 0) |
           (candidate.module.empty() ? 0 : module_bit);
}

/// The search for a file's counterparts: each way of pairing adds the
/// candidates it finds to `found`, with what pairs them.
struct Pairing {
    query::Context& ctx;
    FileTable& files;
    const DependencyGraph& graph;
    llvm::ArrayRef<Fid> members;
    Fid file;
    CanonicalRef self;
    Side side;

    llvm::DenseMap<Fid, Side> sides{};
    llvm::DenseMap<Fid, const index::Shard*> shards{};
    llvm::DenseMap<Fid, CounterpartCandidate> found{};

    /// How many of the file's declarations (on the interface side) or
    /// definitions (on the implementation side) some candidate shares.
    std::uint32_t paired = 0;

    Side side_of(Fid other) {
        auto [it, inserted] = sides.try_emplace(other);
        if(inserted) {
            it->second = classify(files, graph, other);
        }
        return it->second;
    }

    const index::Shard* shard_of(Fid other) {
        auto [it, inserted] = shards.try_emplace(other);
        if(inserted) {
            it->second = ctx.query.declaring_shard(other);
        }
        return it->second;
    }

    /// The module declaration of a unit's text as the build last saw it.
    const ScanResult* declaration_of(Fid unit) {
        auto hash = files.disk.seen_hash(unit);
        if(!hash) {
            return nullptr;
        }
        auto& scans = graph.quick_scans.results;
        auto it = scans.find({unit, *hash});
        return it != scans.end() ? it->second.get() : nullptr;
    }

    /// Directory steps from the file's directory to `other`'s.
    std::uint32_t distance_to(CanonicalRef other) const {
        auto from = path::components(path::parent(self));
        auto to = path::components(path::parent(other));
        auto [a, b] = ranges::mismatch(from, to);
        return static_cast<std::uint32_t>(ranges::distance(a, from.end()) +
                                          ranges::distance(b, to.end()));
    }

    /// The files on the other side sharing the file's declarations and
    /// definitions: where each declaration is defined, and which interface
    /// files declare a definition's symbol — open buffers, and the files
    /// the symbol record lists as referencing it.
    void pair_by_declarations() {
        auto* own = shard_of(file);
        if(!own) {
            return;
        }
        constexpr std::uint8_t declares = 1;
        constexpr std::uint8_t defines = 2;
        llvm::DenseMap<index::SymbolHash, std::uint8_t> written;
        own->for_each_relation([&](index::SymbolHash hash, const index::Relation& relation) {
            if(relation.kind == RelationKind::Declaration) {
                written[hash] |= declares;
            } else if(relation.kind == RelationKind::Definition) {
                written[hash] |= defines;
            }
            return true;
        });
        // The kinds whose declaration and definition can sit in separate
        // files.
        constexpr SymbolKind separable[] = {
            SymbolKind::Class,
            SymbolKind::Struct,
            SymbolKind::Union,
            SymbolKind::Enum,
            SymbolKind::Function,
            SymbolKind::Method,
            SymbolKind::Operator,
            SymbolKind::Variable,
        };
        for(auto [hash, bits]: written) {
            auto symbol = ctx.query.symbol_info(hash, file);
            if(!symbol || !ranges::contains(separable, symbol->kind)) {
                continue;
            }
            if(side == Side::Interface) {
                if(bits != declares) {
                    continue;
                }
                auto definer = ctx.query.definition_file(hash);
                if(definer.valid() && definer != file && side_of(definer) == Side::Implementation) {
                    found[definer].overlap += 1;
                    paired += 1;
                }
                continue;
            }
            if(!(bits & defines)) {
                continue;
            }
            llvm::SmallDenseSet<Fid, 4> declaring;
            auto consider = [&](Fid other) {
                return other != file && side_of(other) == Side::Interface;
            };
            ctx.query.for_each_live_file(hash, RelationKind::Declaration, [&](Fid other) {
                if(consider(other)) {
                    declaring.insert(other);
                }
            });
            ctx.query.project().for_each_reference_file(hash, [&](Fid other) {
                auto* other_shard = consider(other) ? shard_of(other) : nullptr;
                if(!other_shard) {
                    return;
                }
                other_shard->relations_of(hash,
                                          RelationKind::Declaration,
                                          [&](const index::Relation&) {
                                              declaring.insert(other);
                                              return false;
                                          });
            });
            for(auto other: declaring) {
                found[other].overlap += 1;
            }
            paired += declaring.empty() ? 0 : 1;
        }
    }

    /// A module implementation unit pairs with the interface of its module,
    /// a primary module interface with every unit implementing it.
    void pair_by_module() {
        if(auto* own = declaration_of(file); own && own->is_implementation_unit()) {
            for(auto interface: graph.lookup_module(own->module_name)) {
                found[interface].module = own->module_name;
            }
            return;
        }
        auto provided = graph.module_of(file);
        if(provided.empty() || provided.contains(':')) {
            return;
        }
        for(auto unit: members) {
            auto* declared = declaration_of(unit);
            if(declared && declared->is_implementation_unit() &&
               declared->module_name == provided) {
                found[unit].module = provided.str();
            }
        }
    }

    /// The project's files of the same name on the other side: in the
    /// file's own directory, or elsewhere in the workspace when one of the
    /// two includes the other — a header under `include/` and the source
    /// under `src/` including it, not two files of separate parts sharing a
    /// name, nor a system header and a project source. Of those the
    /// nearest, and any other the evidence so far already holds.
    void pair_by_name() {
        auto stem = path::stem(self);
        auto directory = path::parent(self);
        auto root = ctx.root;
        bool rooted = !root.empty() && path::is_within(self, root);
        llvm::SmallVector<std::pair<Fid, std::uint32_t>> named;
        for(auto other: graph.all_files()) {
            auto other_path = files.resolve(other);
            if(other == file || !llvm::StringRef(path::stem(other_path)).equals_insensitive(stem)) {
                continue;
            }
            auto other_side = side_of(other);
            if(other_side == Side::Fragment || other_side == side) {
                continue;
            }
            bool linked =
                graph.count_includes(file, other) > 0 || graph.count_includes(other, file) > 0;
            if(path::parent(other_path) != directory &&
               !(rooted && path::is_within(other_path, root) && linked)) {
                continue;
            }
            // Includers keep naming a header deleted from disk.
            if(!fs::is_file(other_path)) {
                continue;
            }
            named.emplace_back(other, distance_to(other_path));
        }
        auto nearest = std::numeric_limits<std::uint32_t>::max();
        for(auto distance: views::values(named)) {
            nearest = std::min(nearest, distance);
        }
        for(auto [other, distance]: named) {
            if(distance == nearest || found.contains(other)) {
                found[other].same_name = true;
            }
        }
    }
};

}  // namespace

CounterpartRanking rank_counterparts(std::vector<CounterpartCandidate> candidates) {
    ranges::sort(candidates, [](const CounterpartCandidate& a, const CounterpartCandidate& b) {
        if(signals(a) != signals(b)) {
            return signals(a) > signals(b);
        }
        if(a.overlap != b.overlap) {
            return a.overlap > b.overlap;
        }
        if(a.distance != b.distance) {
            return a.distance < b.distance;
        }
        return a.path < b.path;
    });
    auto beats = [](const CounterpartCandidate& a, const CounterpartCandidate& b) {
        auto has = signals(a) & ~module_bit;
        auto other = signals(b) & ~module_bit;
        if((has & other) != other || a.overlap < b.overlap) {
            return false;
        }
        if(has != other) {
            return true;
        }
        if(b.overlap > 0) {
            return a.overlap >= 2 * b.overlap;
        }
        return !a.module.empty() && b.module.empty();
    };
    bool decisive =
        !candidates.empty() &&
        ranges::all_of(views::drop(candidates, 1), [&](const CounterpartCandidate& candidate) {
            return beats(candidates.front(), candidate);
        });
    return {.candidates = std::move(candidates), .decisive = decisive};
}

query::Outcome<CounterpartsResult> counterparts(query::Context& ctx,
                                                const DependencyGraph& graph,
                                                llvm::ArrayRef<Fid> members,
                                                const Spelling& path) {
    if(!fs::is_file(path)) {
        return std::unexpected(std::format("no such file: {}", path));
    }
    auto& files = ctx.query.file_table();
    auto file = files.intern(path);
    CounterpartsResult result{.file = files.display(file)};
    Pairing pairing{
        .ctx = ctx,
        .files = files,
        .graph = graph,
        .members = members,
        .file = file,
        .self = files.resolve(file),
        .side = classify(files, graph, file),
    };
    if(pairing.side == Side::Fragment) {
        return result;
    }
    if(!ctx.query.indexes(file)) {
        ctx.unindexed.push_back(result.file);
    }
    pairing.pair_by_declarations();
    pairing.pair_by_module();
    pairing.pair_by_name();

    std::vector<CounterpartCandidate> candidates;
    for(auto& [other, candidate]: pairing.found) {
        auto other_path = files.resolve(other);
        if(!fs::is_file(other_path)) {
            continue;
        }
        candidate.path = files.display(other);
        candidate.distance = pairing.distance_to(other_path);
        candidates.push_back(std::move(candidate));
    }
    auto ranking = rank_counterparts(std::move(candidates));
    bool interface = pairing.side == Side::Interface;
    for(auto& candidate: ranking.candidates) {
        CounterpartEntry entry{.path = std::move(candidate.path)};
        if(candidate.overlap > 0) {
            entry.reasons.push_back(std::format("{} {} of {} {}{}",
                                                interface ? "defines" : "declares",
                                                candidate.overlap,
                                                pairing.paired,
                                                interface ? "declaration" : "definition",
                                                pairing.paired == 1 ? "" : "s"));
        }
        if(candidate.same_name) {
            entry.reasons.emplace_back("same name");
        }
        if(!candidate.module.empty()) {
            entry.reasons.push_back(
                interface ? std::format("implements module {}", candidate.module)
                          : std::format("interface of module {}", candidate.module));
        }
        result.candidates.push_back(std::move(entry));
    }
    if(ranking.decisive) {
        result.preferred = result.candidates.front().path;
    }
    return result;
}

}  // namespace clice::feature
