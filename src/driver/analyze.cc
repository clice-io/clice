#include <memory>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "analysis/annotation.h"
#include "analysis/module_graph.h"
#include "driver/driver.h"
#include "project/command_resolver.h"
#include "project/open_index.h"
#include "project/project.h"
#include "vfs/file_system.h"

#include "kota/codec/json/json.h"
#include "kota/support/glob_pattern.h"
#include "llvm/ADT/StringExtras.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct ModulesOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

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

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "What to show: overview (default), edge, module, file, obstacles, macros, impact",
           required = false)
    <std::string> view;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "edge: the module whose files name the other's entities",
           required = false)
    <std::string> from;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "edge: the module named", required = false)
    <std::string> to;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "module: the module to show", required = false)
    <std::string> module;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "file: the file to show, relative to the workspace",
           required = false)
    <std::string> file;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Comma-separated globs over workspace-relative paths: analyze only the files "
               "they match (default: every indexed file under the workspace outside a dot "
               "directory)",
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

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off (default: warn)",
           required = false)
    <std::string> log_level;
};

auto make_modules_command() {
    return kota::deco::cli::command<ModulesOptions>("clice analyze modules [OPTIONS]");
}

struct Failure {
    std::string error;
};

std::vector<std::string> comma_list(llvm::StringRef text) {
    llvm::SmallVector<llvm::StringRef> parts;
    text.split(parts, ',', -1, false);
    std::vector<std::string> items;
    for(auto part: parts) {
        items.push_back(part.trim().str());
    }
    return items;
}

struct PartitionFile {
    struct Module {
        std::string name;
        std::vector<std::string> files;
    };

    std::vector<Module> modules;
};

std::expected<analysis::PartitionSpec, std::string> partition_spec(const ModulesOptions& opts) {
    analysis::PartitionSpec spec{
        .depth = static_cast<std::uint32_t>(std::max(opts.depth.value_or(0), 0)),
        .moves = comma_list(opts.move.value_or("")),
        .merges = comma_list(opts.merge.value_or("")),
    };
    if(!opts.partition) {
        return spec;
    }
    auto buffer = vfs::read(*opts.partition);
    if(!buffer) {
        return std::unexpected(
            std::format("cannot read {}: {}", *opts.partition, buffer.error().message()));
    }
    PartitionFile file;
    if(auto result = kota::codec::json::from_string((*buffer)->getBuffer(), file); !result) {
        return std::unexpected(
            std::format("{} is not a partition file: {}", *opts.partition, result.error().message));
    }
    for(auto& module: file.modules) {
        spec.modules.emplace_back(std::move(module.name), std::move(module.files));
    }
    return spec;
}

int run_modules(const ModulesOptions& opts) {
    auto fail = [](std::string error) {
        print_json(Failure{.error = std::move(error)});
        return 1;
    };

    std::vector<kota::GlobPattern> scope;
    for(auto& pattern: comma_list(opts.scope.value_or(""))) {
        auto glob = kota::GlobPattern::create(pattern);
        if(!glob) {
            return fail(
                std::format("invalid --scope glob '{}': {}", pattern, glob.error().message));
        }
        scope.push_back(std::move(*glob));
    }
    auto spec = partition_spec(opts);
    if(!spec) {
        return fail(spec.error());
    }

    auto spelling = workspace_spelling(opts.workspace.value_or(""));
    CanonicalPath root(spelling);
    FileTable files;
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    if(!load_index(project,
                   commands,
                   root,
                   opts.configuration.value_or(""),
                   /*with_build=*/false)) {
        return fail("no usable index; run `clice index` first");
    }

    auto facts = analysis::collect(project, [&](llvm::StringRef path) {
        if(!scope.empty()) {
            return llvm::any_of(scope, [&](auto& glob) { return glob.match(path); });
        }
        return !path.starts_with(".") && !path.contains("/.");
    });
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
        if(auto churn = analysis::git_churn(llvm::StringRef(root), since)) {
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
    } else {
        return fail(std::format("unknown view {}", view));
    }
    return 0;
}

}  // namespace

void add_analyze(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto modules = make_modules_command();
    modules
        .matchAll([&exit_code](ModulesOptions opts) {
            if(opts.help) {
                auto help = make_modules_command();
                print_usage(help);
                exit_code = 0;
                return;
            }
            if(!apply_log_level(opts.log_level.value_or("warn")))
                return;
            logging::stderr_logger("analyze", logging::options);
            exit_code = run_modules(opts);
        })
        .on_error([](auto err) { print_json(Failure{.error = err.message}); });

    auto analyze = std::make_shared<kota::deco::cli::SubCommander>(
        "clice analyze <command> [<args>]",
        "Analyses whose facts an agent turns into refactoring decisions");
    auto usage = [commander = analyze.get(), &exit_code] {
        std::println("usage: clice analyze <command> [<args>]\n");
        print_usage(*commander);
        exit_code = 0;
    };
    analyze->add({.name = "modules",
                  .description = "Dependencies between directories, toward a module partition"},
                 std::move(modules));
    analyze->when_err([usage](auto err) {
        if(err.type == kota::deco::cli::SubCommandError::Type::MissingSubCommand) {
            usage();
        } else {
            LOG_ERROR("{}", err.message);
        }
    });

    root.add({.name = "analyze", .description = "Analyze the indexed workspace for refactoring"},
             [analyze, usage](std::span<std::string> args) {
                 if(!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
                     usage();
                     return;
                 }
                 (*analyze)(args);
             });
}

}  // namespace clice::driver
