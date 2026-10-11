module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :analysis.annotation;
import :analysis.module_graph;
import :analysis.wrapping;
import :driver.driver;
import :index.query;
import :index.symbol_query;
import :project.command_resolver;
import :project.project;
import :server.editor_context;
import :server.query_commands;
import :support.text;

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct ModulesOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to read, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(
        style = KVStyle::JoinedOrSeparate,
        help =
            "What to show: overview (default), edge, module, file, obstacles, macros, impact, interface",
        required = false)
    <std::string> view;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "edge: the module whose files name the other's entities",
           required = false)
    <std::string> from;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "edge: the module named", required = false)
    <std::string> to;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "module, interface: the module to show",
           required = false)
    <std::string> module;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "file: the file to show, relative to the workspace",
           required = false)
    <std::string> file;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Comma-separated globs over workspace-relative paths, absolute ones outside "
               "the workspace: analyze only the files they match (default: every indexed file "
               "under the workspace outside a dot directory)",
           required = false)
    <std::string> scope;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Directory segments naming a file's default module (default 0: its whole "
               "directory)",
           required = false)
    <int> depth;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "JSON file assigning modules by globs, first match wins: "
               R"({"modules": [{"name": "core", "files": ["src/support/**"]}]})",
           required = false)
    <std::string> partition;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "libc++'s module sources (share/libc++/v1), for a partition module provided "
               "by std.compat",
           required = false)
    <std::string> std;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Comma-separated hypothetical moves: <path>=<module>",
           required = false)
    <std::string> move;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Comma-separated hypothetical merges: <module>+<module>[+...] into the first",
           required = false)
    <std::string> merge;

    DecoKV(names = {"--move-entity", "--move-entity="},
           style = KVStyle::JoinedOrSeparate,
           help = "A hypothetical declaration move: <locator>=<header>",
           required = false)
    <std::vector<std::string>> move_entity;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Comma-separated annotation files weighing the results: "
               R"({"name": "compile_time", "unit": "s", "values": {"src/a.cpp": 1.5}})",
           required = false)
    <std::string> annotation;

    DecoKV(names = {"--churn-since", "--churn-since="},
           style = KVStyle::JoinedOrSeparate,
           help =
               "git log --since value of the churn annotation (default 6.months; empty "
               "leaves it out)",
           required = false)
    <std::string> churn_since;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Most hotspots, move and split candidates the overview lists (default 20)",
           required = false)
    <int> limit;

    LogLevelOption log{.log_level = LogLevel::Warn};
};

int run_modules(const ModulesOptions& opts) {
    auto fail = [](std::string error) {
        print_json(Failure{.error = std::move(error)});
        return 1;
    };

    auto libcxx = analysis::read_std_modules(opts.std.value_or(""));
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = analysis::read_partition(
        {
            .depth = static_cast<std::uint32_t>(std::max(opts.depth.value_or(0), 0)),
            .moves = comma_list(opts.move.value_or("")),
            .merges = comma_list(opts.merge.value_or("")),
        },
        opts.partition.value_or(""),
        *libcxx);
    if(!spec) {
        return fail(spec.error());
    }
    std::vector<query::SymbolEntry> candidates;
    auto move = [&](Project& project,
                    CommandResolver& commands,
                    analysis::Facts& facts) -> std::expected<void, std::string> {
        ContextsBlob saved;
        EditorContext contexts{project, commands, saved};
        index::IndexQuery index_query(project.project_index, project.file_table, nullptr, nullptr);
        query::Context ctx{.project = project, .contexts = contexts, .query = index_query};
        for(llvm::StringRef spec: opts.move_entity.value_or(std::vector<std::string>{})) {
            auto [text, path] = spec.rsplit('=');
            if(text.empty() || path.empty()) {
                return std::unexpected(std::format("--move-entity {}: expected <locator>=<header>",
                                                   std::string_view(spec)));
            }
            auto locator = index::SymbolQuery::parse(text);
            if(!locator) {
                return std::unexpected(
                    std::format("--move-entity {}: {}", std::string_view(spec), locator.error()));
            }
            auto symbols = query::declaration(ctx, std::move(*locator));
            if(!symbols) {
                candidates = std::move(ctx.candidates);
                return std::unexpected(
                    std::format("--move-entity {}: {}", std::string_view(spec), symbols.error()));
            }
            if(auto moved = analysis::move_entities(facts, spec, *symbols, path); !moved) {
                return moved;
            }
        }
        return {};
    };
    auto loaded = analysis::load_facts(workspace_spelling(opts.workspace.value_or("")),
                                       opts.configuration.value_or(""),
                                       comma_list(opts.scope.value_or("")),
                                       move);
    if(!loaded) {
        Failure failure{.error = std::move(loaded.error().message),
                        .stale = std::move(loaded.error().dropped)};
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

    auto view = opts.view.value_or("overview");
    // Only these views weigh by annotations, and git history is not free.
    bool weighted = view == "overview" || view == "impact" || view == "file";
    analysis::Annotations annotations;
    if(auto since = opts.churn_since.value_or("6.months"); weighted && !since.empty()) {
        // A workspace outside git history has no churn; the results stay
        // unweighted by it rather than failing.
        if(auto churn = analysis::git_churn(loaded->root, since)) {
            annotations.list.push_back(std::move(*churn));
        } else {
            LOG_WARN("no churn annotation: {}", churn.error());
        }
    }
    for(auto& path: comma_list(opts.annotation.value_or(""))) {
        auto annotation = analysis::read_annotation(path);
        if(!annotation) {
            return fail(annotation.error());
        }
        // A given annotation replaces the computed one of its name.
        std::erase_if(annotations.list,
                      [&](auto& known) { return known.name == annotation->name; });
        annotations.list.push_back(std::move(*annotation));
    }

    analysis::Report report{.facts = facts, .partition = *partition, .annotations = annotations};
    auto print = [&](auto result) {
        if(!result) {
            return fail(result.error());
        }
        print_json(*result);
        return 0;
    };
    if(view == "overview") {
        print_json(
            report.overview(static_cast<std::uint32_t>(std::max(opts.limit.value_or(20), 0))));
    } else if(view == "edge") {
        if(!opts.from || !opts.to) {
            return fail("--view edge needs --from and --to");
        }
        return print(report.edge(*opts.from, *opts.to));
    } else if(view == "module") {
        if(!opts.module) {
            return fail("--view module needs --module");
        }
        return print(report.module(*opts.module));
    } else if(view == "file") {
        if(!opts.file) {
            return fail("--view file needs --file");
        }
        return print(report.file(*opts.file));
    } else if(view == "obstacles") {
        print_json(report.obstacles());
    } else if(view == "macros") {
        print_json(report.macros());
    } else if(view == "impact") {
        print_json(report.impact());
    } else if(view == "interface") {
        return print(report.interface(opts.module.value_or("")));
    } else {
        return fail(std::format("unknown view {}", view));
    }
    return 0;
}

CommandHandler build(const char*) {
    auto modules = kota::deco::cli::command<ModulesOptions>("clice analyze modules [OPTIONS]");
    modules
        .match_all([](ModulesOptions opts) {
            opts.log.apply();
            logging::stderr_logger("analyze", logging::options);
            return run_modules(opts);
        })
        .on_error([](auto err) { print_json(Failure{.error = err.message}); });

    auto analyze = std::make_shared<kota::deco::cli::SubCommander>(
        "clice analyze <command> [<args>]",
        "Analyses whose facts an agent turns into refactoring decisions");
    analyze->add({.name = "modules",
                  .description = "Dependencies between directories, toward a module partition"},
                 std::move(modules));
    analyze->enable_help().when_err(subcommand_error_handler(*analyze));
    return [analyze](std::span<std::string> args) {
        return (*analyze)(args);
    };
}

[[maybe_unused]] const bool registered = register_command(
    Command::analyze,
    {.description = "Analyze the indexed workspace for refactoring", .build = build});

}  // namespace

}  // namespace clice::driver
