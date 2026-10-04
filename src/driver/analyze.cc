#include <memory>
#include <span>
#include <string>
#include <vector>

#include "analysis/annotation.h"
#include "analysis/module_graph.h"
#include "driver/analysis_support.h"
#include "driver/driver.h"
#include "driver/query_support.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/StringExtras.h"

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
           help = "Comma-separated hypothetical declaration moves: <name or #id>=<header>",
           required = false)
    <std::string> move_entity;

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

    auto libcxx = read_std(opts.std.value_or(""));
    if(!libcxx) {
        return fail(libcxx.error());
    }
    auto spec = read_partition(
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
    auto loaded = load_facts(opts.workspace.value_or(""),
                             opts.configuration.value_or(""),
                             opts.scope.value_or(""));
    if(!loaded) {
        print_json(loaded.error());
        return 1;
    }
    auto& facts = loaded->facts;
    for(auto& move: comma_list(opts.move_entity.value_or(""))) {
        if(auto moved = analysis::move_entities(facts, move); !moved) {
            return fail(moved.error());
        }
    }
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

}  // namespace

void add_analyze(kota::deco::cli::SubCommander& root) {
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

    root.add({.name = "analyze", .description = "Analyze the indexed workspace for refactoring"},
             [analyze](std::span<std::string> args) { return (*analyze)(args); });
}

}  // namespace clice::driver
