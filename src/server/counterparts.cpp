module;

#include "modules/prelude.h"

module clice;

import :command.command;
import :index.project_index;
import :index.query;
import :index.shard;
import :index.types;
import :project.project;
import :semantic.symbol;
import :server.counterparts;
import :syntax.dependency_graph;
import :syntax.scan;
import :vfs.file_system;
import :vfs.path;

namespace clice::query {

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

Side classify(Project& project, Fid file) {
    llvm::StringRef path = project.file_table.resolve(file);
    auto extension = path::extension(path);
    if(extension == ".inc" || extension == ".def") {
        return Side::Fragment;
    }
    if(is_context_header_path(path)) {
        return Side::Implementation;
    }
    if(is_header_path(path) || !project.dep_graph.module_of(file).empty()) {
        return Side::Interface;
    }
    return Side::Implementation;
}

/// The kinds whose declaration and definition can sit in separate files.
bool separable(SymbolKind kind) {
    return kind == SymbolKind::Class || kind == SymbolKind::Struct || kind == SymbolKind::Union ||
           kind == SymbolKind::Enum || kind == SymbolKind::Function || kind == SymbolKind::Method ||
           kind == SymbolKind::Operator || kind == SymbolKind::Variable;
}

/// Directory steps from one directory to another.
std::uint32_t steps(llvm::StringRef from, llvm::StringRef to) {
    auto a = path::begin(from);
    auto b = path::begin(to);
    while(a != path::end(from) && b != path::end(to) && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<std::uint32_t>(std::distance(a, path::end(from)) +
                                      std::distance(b, path::end(to)));
}

constexpr unsigned shared_bit = 4;
constexpr unsigned name_bit = 2;
constexpr unsigned module_bit = 1;

/// The kinds of evidence a candidate has as bits, a stronger kind a higher
/// bit.
unsigned signals(const Evidence& candidate) {
    return (candidate.overlap > 0 ? shared_bit : 0) | (candidate.same_name ? name_bit : 0) |
           (candidate.module.empty() ? 0 : module_bit);
}

struct Pairing {
    Context& ctx;
    Fid file;
    std::string self;
    Side side;

    llvm::DenseMap<Fid, Side> sides{};
    llvm::DenseMap<Fid, const index::Shard*> rows{};
    llvm::MapVector<Fid, Evidence> found{};

    /// How many of the file's declarations (on the interface side) or
    /// definitions (on the implementation side) some candidate shares.
    std::uint32_t paired = 0;

    Side side_of(Fid other) {
        auto [it, inserted] = sides.try_emplace(other);
        if(inserted) {
            it->second = classify(ctx.project, other);
        }
        return it->second;
    }

    const index::Shard* rows_of(Fid other) {
        auto [it, inserted] = rows.try_emplace(other);
        if(inserted) {
            auto source = ctx.query.serving(other);
            it->second = source ? source->rows : nullptr;
        }
        return it->second;
    }

    /// The module declaration of a unit's text as the build last saw it.
    const ScanResult* declaration_of(Fid unit) {
        auto hash = ctx.project.file_table.seen_hash(unit);
        if(!hash) {
            return nullptr;
        }
        auto& scans = ctx.project.dep_graph.quick_scans.results;
        auto it = scans.find({unit, *hash});
        return it != scans.end() ? &it->second : nullptr;
    }

    /// The declarations and definitions the file shares with files on the
    /// other side: where each declaration is defined, and which interface
    /// files referencing a definition's symbol declare it.
    void overlap() {
        auto source = ctx.query.serving(file);
        if(!source) {
            return;
        }
        constexpr std::uint8_t declares = 1;
        constexpr std::uint8_t defines = 2;
        llvm::DenseMap<index::SymbolHash, std::uint8_t> written;
        source->rows->for_each_relation(
            [&](index::SymbolHash hash, const index::Relation& relation) {
                if(relation.kind == RelationKind::Declaration) {
                    written[hash] |= declares;
                } else if(relation.kind == RelationKind::Definition) {
                    written[hash] |= defines;
                }
                return true;
            });
        auto& table = ctx.project.project_index;
        for(auto [hash, bits]: written) {
            auto identity = table.identity_of(hash);
            if(!identity || !separable(identity->kind)) {
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
            bool declared = false;
            table.each_reference_file(hash, [&](Fid other) {
                if(other == file || side_of(other) != Side::Interface) {
                    return;
                }
                auto* other_rows = rows_of(other);
                if(!other_rows) {
                    return;
                }
                bool declaring = false;
                other_rows->lookup(hash, RelationKind::Declaration, [&](const index::Relation&) {
                    declaring = true;
                    return false;
                });
                if(declaring) {
                    found[other].overlap += 1;
                    declared = true;
                }
            });
            paired += declared ? 1 : 0;
        }
    }

    /// A module implementation unit pairs with the interface of its module,
    /// a primary module interface with every unit implementing it.
    void module_units() {
        auto& graph = ctx.project.dep_graph;
        if(auto* own = declaration_of(file); own && own->is_implementation_unit()) {
            for(auto interface: graph.lookup_module(own->module_name)) {
                found[interface].module = own->module_name;
            }
            return;
        }
        auto provided = graph.module_of(file).str();
        if(provided.empty() || llvm::StringRef(provided).contains(':')) {
            return;
        }
        for(auto unit: ctx.project.build.members()) {
            auto* declared = declaration_of(unit);
            if(declared && declared->is_implementation_unit() &&
               declared->module_name == provided) {
                found[unit].module = provided;
            }
        }
    }

    /// The project's files of the same name on the other side: in the
    /// file's own directory, or elsewhere in the workspace when one of the
    /// two includes the other — a header under `include/` and the source
    /// under `src/` including it, not two files of separate parts sharing a
    /// name, nor a system header and a project source. Of those the
    /// nearest, and any other the evidence so far already holds.
    void same_name() {
        auto& files = ctx.project.file_table;
        auto& graph = ctx.project.dep_graph;
        auto stem = path::stem(self);
        auto directory = path::parent_path(self);
        CanonicalRef root = ctx.project.config.workspace_root;
        bool rooted = !root.empty() && path::under(self, llvm::StringRef(root));
        auto includes = [&](Fid includer, Fid included) {
            return llvm::is_contained(graph.get_all_includes(includer), included);
        };
        llvm::SmallVector<std::pair<Fid, std::uint32_t>> named;
        for(auto other: graph.all_files()) {
            llvm::StringRef other_path = files.resolve(other);
            if(other == file || !path::stem(other_path).equals_insensitive(stem)) {
                continue;
            }
            auto other_side = side_of(other);
            if(other_side == Side::Fragment || other_side == side) {
                continue;
            }
            auto other_directory = path::parent_path(other_path);
            if(other_directory != directory &&
               !(rooted && path::under(other_path, llvm::StringRef(root)) &&
                 (includes(file, other) || includes(other, file)))) {
                continue;
            }
            named.emplace_back(other, steps(directory, other_directory));
        }
        auto nearest = ~0u;
        for(auto distance: llvm::make_second_range(named)) {
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

Ranking rank_counterparts(std::vector<Evidence> candidates) {
    std::ranges::sort(candidates, [](const Evidence& a, const Evidence& b) {
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
    auto beats = [](const Evidence& a, const Evidence& b) {
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
    bool decisive = !candidates.empty() &&
                    llvm::all_of(llvm::drop_begin(candidates), [&](const Evidence& candidate) {
                        return beats(candidates.front(), candidate);
                    });
    return {.candidates = std::move(candidates), .decisive = decisive};
}

Outcome<CounterpartsResult> counterparts(Context& ctx, const Spelling& spelling) {
    if(!vfs::is_file(spelling)) {
        return std::unexpected(std::format("no such file: {}", spelling));
    }
    auto& files = ctx.project.file_table;
    auto file = files.intern(spelling);
    CounterpartsResult result{.file = files.display(file)};
    Pairing pairing{
        .ctx = ctx,
        .file = file,
        .self = llvm::StringRef(files.resolve(file)).str(),
        .side = classify(ctx.project, file),
    };
    if(pairing.side == Side::Fragment) {
        return result;
    }
    pairing.overlap();
    pairing.module_units();
    pairing.same_name();

    std::vector<Evidence> candidates;
    for(auto& [other, evidence]: pairing.found) {
        llvm::StringRef other_path = files.resolve(other);
        if(!vfs::is_file(other_path)) {
            continue;
        }
        evidence.path = files.display(other);
        evidence.distance = steps(path::parent_path(pairing.self), path::parent_path(other_path));
        candidates.push_back(std::move(evidence));
    }
    auto ranking = rank_counterparts(std::move(candidates));
    bool interface = pairing.side == Side::Interface;
    auto noun = [&](llvm::StringRef singular) {
        return pairing.paired == 1 ? singular.str() : singular.str() + "s";
    };
    for(auto& candidate: ranking.candidates) {
        CounterpartEntry entry{.path = std::move(candidate.path)};
        if(candidate.overlap > 0) {
            entry.reasons.push_back(interface ? std::format("defines {} of {} {}",
                                                            candidate.overlap,
                                                            pairing.paired,
                                                            noun("declaration"))
                                              : std::format("declares {} of {} {}",
                                                            candidate.overlap,
                                                            pairing.paired,
                                                            noun("definition")));
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

}  // namespace clice::query
