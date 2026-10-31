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
    DecoKV(help =
               "A glob over paths relative to the project root, absolute ones outside it: "
               "analyze only the files it matches (default: every indexed file under the "
               "project root outside a dot directory)",
           required = false)
    <std::vector<std::string>> scope;

    DecoKV(help =
               "Directory segments naming a file's default module (default 0: its whole "
               "directory)",
           required = false)
    <int> dir_depth;

    DecoKV(help =
               "JSON file assigning modules by globs, first match wins: "
               R"({"modules": [{"name": "core", "files": ["src/support/**"]}]})",
           required = false)
    <std::string> partition;

    DecoKV(help =
               "libc++'s module sources (share/libc++/v1), for a partition module provided "
               "by std.compat",
           required = false)
    <std::string> std;

    DecoKV(help = "A hypothetical move: <path>=<module>", required = false)
    <std::vector<std::string>> move;

    DecoKV(help = "A hypothetical merge: <module>+<module>[+...] into the first", required = false)
    <std::vector<std::string>> merge;

    DecoKV(help = "A hypothetical declaration move: <locator>=<header>", required = false)
    <std::vector<std::string>> move_entity;

    ProjectOptions where;
};

/// What weighs the results of the views that rank by weight.
struct WeightOptions {
    DecoKV(help =
               "An annotation file weighing the results: "
               R"({"name": "compile_time", "unit": "s", "values": {"src/a.cpp": 1.5}})",
           required = false)
    <std::vector<std::string>> annotation;

    DecoKV(help =
               "git log --since value of the churn annotation (default 6.months; empty "
               "leaves it out)",
           required = false)
    <std::string> churn_since;
};

struct OverviewOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(help = "Most hotspots, move and split candidates to list (default 20)", required = false)
    <int> limit;

    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
};

struct ImpactOptions {
    kota::deco::decl::HelpOption help;
    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
};

struct FileViewOptions {
    kota::deco::decl::HelpOption help;
    FileTarget target;
    ModelOptions model;
    WeightOptions weights;
    LogLevelOption log;
};

struct EdgeOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<FROM> <TO>",
              help = "The module whose files name the other's entities, and the module named",
              arg_num = 2)
    <std::vector<std::string>> modules;

    ModelOptions model;
    LogLevelOption log;
};

struct ModuleViewOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<MODULE>", help = "The module")
    <std::string> module;

    ModelOptions model;
    LogLevelOption log;
};

struct InterfaceOptions {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<MODULE>", help = "The module (default: every module)", required = false)
    <std::string> module;

    ModelOptions model;
    LogLevelOption log;
};

struct PlainViewOptions {
    kota::deco::decl::HelpOption help;
    ModelOptions model;
    LogLevelOption log;
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
    auto libcxx = analysis::read_std_modules(model.std.value_or(""));
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = analysis::read_partition(
        {
            .depth = static_cast<std::uint32_t>(std::max(model.dir_depth.value_or(0), 0)),
            .moves = model.move.value_or(std::vector<std::string>{}),
            .merges = model.merge.value_or(std::vector<std::string>{}),
        },
        model.partition.value_or(""),
        *libcxx);
    if(!spec) {
        return fail(spec.error());
    }
    std::vector<feature::query::SymbolEntry> candidates;
    std::vector<std::string> unindexed;
    auto move_declarations = [&](Project& project,
                                 analysis::Facts& facts) -> std::expected<void, std::string> {
        auto ctx = query_context(project);
        for(llvm::StringRef move: model.move_entity.value_or(std::vector<std::string>{})) {
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
    auto loaded = analysis::load_facts(project, model.scope.value_or(std::vector<std::string>{}));
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
        if(auto since = weights->churn_since.value_or("6.months"); !since.empty()) {
            if(auto churn = analysis::git_churn(loaded->root, since)) {
                annotations.list.push_back(std::move(*churn));
            } else {
                LOG_WARN("no churn annotation: {}", churn.error());
            }
        }
        for(auto& path: weights->annotation.value_or(std::vector<std::string>{})) {
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

/// A module view over the options `Options`, which `view` answers from the
/// report.
template <typename Options, typename View>
CommandHandler module_view(std::string_view usage, View view) {
    return operation<Options>(usage, "analyze", Errors::JSON, [view](const Options& opts) {
        const WeightOptions* weights = nullptr;
        if constexpr(requires { opts.weights; }) {
            weights = &opts.weights;
        }
        return run_view(opts.model, weights, [&](analysis::Report& report) {
            return view(report, opts);
        });
    });
}

CommandHandler build() {
    std::vector<Operation> views;
    views.push_back({
        .name = "edge",
        .description = "Every entity an edge between two modules carries, and its uses",
        .run = module_view<EdgeOptions>("clice analyze modules edge <FROM> <TO> [OPTIONS]",
                                        [](analysis::Report& report, const EdgeOptions& opts) {
                                            auto& modules = *opts.modules;
                                            return answer(report.edge(modules[0], modules[1]));
                                        }),
    });
    views.push_back({
        .name = "module",
        .description = "A module's files by their dependencies, the lines it splits along",
        .run = module_view<ModuleViewOptions>(
            "clice analyze modules module <MODULE> [OPTIONS]",
            [](analysis::Report& report, const ModuleViewOptions& opts) {
                return answer(report.module(*opts.module));
            }),
    });
    views.push_back({
        .name = "file",
        .description = "What a file names by module, who names its entities, its annotations",
        .run = module_view<FileViewOptions>(
            "clice analyze modules file <PATH> [OPTIONS]",
            [](analysis::Report& report, const FileViewOptions& opts) {
                return answer(report.file(fact_path(opts.model.where, *opts.target.path)));
            }),
    });
    views.push_back({
        .name = "obstacles",
        .description = "What a mechanical rewrite into modules cannot carry over",
        .run = module_view<PlainViewOptions>("clice analyze modules obstacles [OPTIONS]",
                                             [](analysis::Report& report, const PlainViewOptions&) {
                                                 print_json(report.obstacles());
                                                 return 0;
                                             }),
    });
    views.push_back({
        .name = "macros",
        .description = "The macros crossing modules",
        .run = module_view<PlainViewOptions>("clice analyze modules macros [OPTIONS]",
                                             [](analysis::Report& report, const PlainViewOptions&) {
                                                 print_json(report.macros());
                                                 return 0;
                                             }),
    });
    views.push_back({
        .name = "impact",
        .description = "What a change to each module rebuilds",
        .run = module_view<ImpactOptions>("clice analyze modules impact [OPTIONS]",
                                          [](analysis::Report& report, const ImpactOptions&) {
                                              print_json(report.impact());
                                              return 0;
                                          }),
    });
    views.push_back({
        .name = "interface",
        .description = "Per module, what a wrapper module over its headers needs",
        .run = module_view<InterfaceOptions>(
            "clice analyze modules interface [<MODULE>] [OPTIONS]",
            [](analysis::Report& report, const InterfaceOptions& opts) {
                return answer(report.interface(opts.module.value_or("")));
            }),
    });
    auto overview = module_view<OverviewOptions>(
        "clice analyze modules [OPTIONS]",
        [](analysis::Report& report, const OverviewOptions& opts) {
            print_json(
                report.overview(static_cast<std::uint32_t>(std::max(opts.limit.value_or(20), 0))));
            return 0;
        });

    std::vector<Operation> analyses;
    analyses.push_back({
        .name = "modules",
        .description = "Dependencies between directories, toward a module partition",
        .run = with_operations("", std::move(views), Errors::JSON, std::move(overview)),
    });
    return with_operations("Analyses whose facts an agent turns into refactoring decisions",
                           std::move(analyses),
                           Errors::JSON);
}

[[maybe_unused]] const bool registered = register_command(
    Command::analyze,
    {.description = "Analyze the indexed project for refactoring", .build = build});

}  // namespace

}  // namespace clice::driver
