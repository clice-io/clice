module clice;

import :analysis.annotation;
import :analysis.module_graph;
import :analysis.wrapping;
import :driver.driver;
import :feature.query;
import :index.symbol_query;
import :project.project;
import :vfs.path;

namespace clice::driver {

namespace {

/// What the module views share: the files analyzed, the partition and the
/// hypothetical changes it is evaluated under.
struct ModelOptions {
    KANNOTATE(&scope,
              description =
                  "A glob over paths relative to the project root, absolute ones outside it: "
                  "analyze only the files it matches (default: every indexed file under the "
                  "project root outside a dot directory)")
    std::vector<std::string> scope;

    KANNOTATE(
        &dir_depth,
        description = "Directory segments naming a file's default module (0: its whole directory)")
    int dir_depth = 0;

    KANNOTATE(&partition,
              description =
                  "JSON file assigning modules by globs, first match wins: "
                  R"({"modules": [{"name": "core", "files": ["src/support/**"]}]})")
    std::string partition;

    KANNOTATE(&std,
              description =
                  "libc++'s module sources (share/libc++/v1), for a partition module "
                  "provided by std.compat")
    std::string std;

    KANNOTATE(&move, description = "A hypothetical move: <path>=<module>")
    std::vector<std::string> move;

    KANNOTATE(&merge, description = "A hypothetical merge: <module>+<module>[+...] into the first")
    std::vector<std::string> merge;

    KANNOTATE(&move_entity, description = "A hypothetical declaration move: <locator>=<header>")
    std::vector<std::string> move_entity;

    ProjectOptions where;
};

/// What weighs the results of the views that rank by weight.
struct WeightOptions {
    KANNOTATE(&annotation,
              description =
                  "An annotation file weighing the results: "
                  R"({"name": "compile_time", "unit": "s", "values": {"src/a.cpp": 1.5}})")
    std::vector<std::string> annotation;

    KANNOTATE(&churn_since,
              description = "git log --since value of the churn annotation (empty leaves it out)")
    std::string churn_since = "6.months";
};

KANNOTATE_TYPE(struct Edge,
               description = "Every entity an edge between two modules carries, and its uses");

struct Edge {
    constexpr static std::string_view usage_line =
        "clice analyze modules edge <FROM> <TO> [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    KANNOTATE(
        &modules,
        presence = required,
        cli::positional = true,
        cli::meta_var = "<FROM> <TO>",
        cli::arg_num = 2,
        description = "The module whose files name the other's entities, and the module named")
    std::vector<std::string> modules;

    ModelOptions model;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Module,
               description = "A module's files by their dependencies, the lines it splits along");

struct Module {
    constexpr static std::string_view usage_line =
        "clice analyze modules module <MODULE> [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    KANNOTATE(&module,
              cli::positional = true,
              cli::meta_var = "<MODULE>",
              description = "The module")
    std::string module;

    ModelOptions model;
    LogLevelOption log;
};

KANNOTATE_TYPE(
    struct File,
    description = "What a file names by module, who names its entities, its annotations");

struct File {
    constexpr static std::string_view usage_line = "clice analyze modules file <PATH> [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    FileTarget target;
    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Obstacles,
               description = "What a mechanical rewrite into modules cannot carry over");

struct Obstacles {
    constexpr static std::string_view usage_line = "clice analyze modules obstacles [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    ModelOptions model;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Macros, description = "The macros crossing modules");

struct Macros {
    constexpr static std::string_view usage_line = "clice analyze modules macros [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    ModelOptions model;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Impact, description = "What a change to each module rebuilds");

struct Impact {
    constexpr static std::string_view usage_line = "clice analyze modules impact [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Interface,
               description = "Per module, what a wrapper module over its headers needs");

struct Interface {
    constexpr static std::string_view usage_line =
        "clice analyze modules interface [<MODULE>] [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    KANNOTATE(&module,
              cli::positional = true,
              cli::meta_var = "<MODULE>",
              description = "The module (default: every module)")
    std::optional<std::string> module;

    ModelOptions model;
    LogLevelOption log;
};

KANNOTATE_TYPE(struct Modules,
               description = "Dependencies between directories, toward a module partition");

struct Modules {
    constexpr static std::string_view usage_line = "clice analyze modules [OPTIONS] [<view>]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    KANNOTATE(&limit, description = "Most hotspots, move and split candidates to list")
    int limit = 20;

    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
    std::optional<std::variant<Edge, Module, File, Obstacles, Macros, Impact, Interface>> operation;
};

KANNOTATE_TYPE(struct Analyze, description = "Analyze the indexed project for refactoring");

struct Analyze {
    constexpr static std::string_view usage_line = "clice analyze <analysis> [<args>]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    std::optional<std::variant<Modules>> operation;
};

/// A file as the facts name it: one under the project by its path from
/// the root, one outside by its identity; `argument` is relative to the
/// current directory or absolute. The view runs once the root was found.
std::string fact_path(const ProjectOptions& where, const std::string& argument) {
    CanonicalPath file(Spelling(argument, Spelling::cwd()));
    CanonicalPath root(*where.root());
    if(!path::is_within(file, root)) {
        return file.str();
    }
    return llvm::StringRef(file).drop_front(llvm::StringRef(root).size()).ltrim('/').str();
}

template <typename T>
int answer(const std::expected<T, std::string>& result) {
    if(!result) {
        return fail(result.error());
    }
    print_json(*result);
    return 0;
}

/// Build the report `model` describes and hand it to `view`. Only the
/// views weighing by annotations take `weights`: git history is not free.
template <typename View>
int run_view(const ModelOptions& model, const WeightOptions* weights, View view) {
    auto libcxx = analysis::read_std_modules(model.std);
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = analysis::read_partition(
        {
            .depth = static_cast<std::uint32_t>(std::max(model.dir_depth, 0)),
            .moves = model.move,
            .merges = model.merge,
        },
        model.partition,
        *libcxx);
    if(!spec) {
        return fail(spec.error());
    }
    std::vector<feature::query::SymbolEntry> candidates;
    std::vector<std::string> unindexed;
    auto move_declarations = [&](Project& project,
                                 analysis::Facts& facts) -> std::expected<void, std::string> {
        auto ctx = query_context(project);
        for(llvm::StringRef move: model.move_entity) {
            auto [text, path] = move.rsplit('=');
            if(text.empty() || path.empty()) {
                return std::unexpected(std::format("--move-entity {}: expected <locator>=<header>",
                                                   std::string_view(move)));
            }
            auto locator = index::SymbolQuery::parse(text);
            if(!locator) {
                return std::unexpected(
                    std::format("--move-entity {}: {}", std::string_view(move), locator.error()));
            }
            auto symbols = feature::query::declaration(ctx, std::move(*locator));
            if(!symbols) {
                candidates = std::move(ctx.candidates);
                unindexed = std::move(ctx.unindexed);
                return std::unexpected(
                    std::format("--move-entity {}: {}", std::string_view(move), symbols.error()));
            }
            if(auto moved = analysis::move_entities(facts, move, *symbols, path); !moved) {
                return moved;
            }
        }
        return {};
    };
    auto opened = open_for_analysis(model.where);
    if(!opened) {
        return fail(std::move(opened.error()));
    }
    auto& project = (*opened)->project;
    auto loaded = analysis::load_facts(project, model.scope);
    // The locators resolve against the index the facts were read from.
    if(loaded) {
        if(auto moved = move_declarations(project, loaded->facts); !moved) {
            loaded = std::unexpected(analysis::LoadError{.message = std::move(moved.error())});
        }
    }
    if(!loaded) {
        Failure failure{.error = std::move(loaded.error().message),
                        .stale = std::move(loaded.error().dropped)};
        failure.stale.insert(failure.stale.end(), unindexed.begin(), unindexed.end());
        if(!candidates.empty()) {
            failure.candidates = std::move(candidates);
        }
        print_json(failure);
        return 1;
    }
    auto& facts = loaded->facts;
    auto partition = analysis::partition(facts, *spec);
    if(!partition) {
        return fail(partition.error());
    }

    analysis::Annotations annotations;
    if(weights) {
        // A project outside git history has no churn; the results stay
        // unweighted by it rather than failing.
        if(auto& since = weights->churn_since; !since.empty()) {
            if(auto churn = analysis::git_churn(loaded->root, since)) {
                annotations.list.push_back(std::move(*churn));
            } else {
                LOG_WARN("no churn annotation: {}", churn.error());
            }
        }
        for(auto& path: weights->annotation) {
            auto annotation = analysis::read_annotation(path);
            if(!annotation) {
                return fail(annotation.error());
            }
            // A given annotation replaces the computed one of its name.
            std::erase_if(annotations.list,
                          [&](auto& known) { return known.name == annotation->name; });
            annotations.list.push_back(std::move(*annotation));
        }
    }
    analysis::Report report{.facts = facts, .partition = *partition, .annotations = annotations};
    return view(report);
}

/// A module view: `view` answers from the report `options` describe.
template <typename Options, typename View>
int module_view(const Options& options, View view) {
    const WeightOptions* weights = nullptr;
    if constexpr(requires { options.weights; }) {
        weights = &options.weights;
    }
    return run_view(options.model, weights, view);
}

int run(Modules& modules) {
    return module_view(modules, [&](analysis::Report& report) {
        print_json(report.overview(static_cast<std::uint32_t>(std::max(modules.limit, 0))));
        return 0;
    });
}

int run(Edge& edge) {
    return module_view(edge, [&](analysis::Report& report) {
        return answer(report.edge(edge.modules[0], edge.modules[1]));
    });
}

int run(Module& module) {
    return module_view(module, [&](analysis::Report& report) {
        return answer(report.module(module.module));
    });
}

int run(File& file) {
    return module_view(file, [&](analysis::Report& report) {
        return answer(report.file(fact_path(file.model.where, file.target.path)));
    });
}

int run(Obstacles& obstacles) {
    return module_view(obstacles, [](analysis::Report& report) {
        print_json(report.obstacles());
        return 0;
    });
}

int run(Macros& macros) {
    return module_view(macros, [](analysis::Report& report) {
        print_json(report.macros());
        return 0;
    });
}

int run(Impact& impact) {
    return module_view(impact, [](analysis::Report& report) {
        print_json(report.impact());
        return 0;
    });
}

int run(Interface& options) {
    return module_view(options, [&](analysis::Report& report) {
        return answer(report.interface(options.module.value_or("")));
    });
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Analyze>);

}  // namespace

}  // namespace clice::driver
