module;

#include "modules/prelude.h"

module clice;

import :config.config;
import :driver.driver;
import :index.symbol_query;
import :project.configuration;
import :project.open_index;
import :server.counterparts;
import :server.query_commands;

namespace clice::driver {

namespace {

/// What every question takes.
struct QueryOptions {
    WorkspaceOptions where;
    FreshOption fresh;
};

struct FileQuestion {
    kota::deco::decl::HelpOption help;
    FileTarget target;
    QueryOptions query;
    LogLevelOption log;
};

struct FileDepsQuestion {
    kota::deco::decl::HelpOption help;
    FileTarget target;

    DecoKV(help = "includes, includers or both (default)", required = false)
    <std::string> direction;

    DecoKV(help = "Levels to follow (default 1, 0 for all)", required = false)
    <int> depth;

    QueryOptions query;
    LogLevelOption log;
};

struct ProjectFilesQuestion {
    kota::deco::decl::HelpOption help;

    DecoKV(help = "all (default), source, header or module", required = false)
    <std::string> filter;

    QueryOptions query;
    LogLevelOption log;
};

struct SearchQuestion {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<LOCATOR>",
              help = "The symbols to list, as a locator (default: every symbol)",
              required = false)
    <std::vector<std::string>> locator;

    DecoKV(help = "At most this many symbols (default 100)", required = false)
    <int> limit;

    QueryOptions query;
    LogLevelOption log;
};

struct SymbolQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;
    QueryOptions query;
    LogLevelOption log;
};

struct ReferencesQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoFlag(names = {"--include-declaration"},
             help = "List the declarations and the definition too",
             required = false)
    include_declaration;

    QueryOptions query;
    LogLevelOption log;
};

struct CallGraphQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoKV(help = "callers, callees or both (default)", required = false)
    <std::string> direction;

    QueryOptions query;
    LogLevelOption log;
};

struct TypeHierarchyQuestion {
    kota::deco::decl::HelpOption help;
    SymbolTarget target;

    DecoKV(help = "supertypes, subtypes or both (default)", required = false)
    <std::string> direction;

    QueryOptions query;
    LogLevelOption log;
};

/// Answer a question against the opened index: `ask` asks it of the query
/// context. The build questions (`with_build`) walk manifests and
/// contexts, which only the writer's load restores; the index questions
/// bind the tables in place and touch nothing else. The answer lists as
/// stale the files withheld on the way, the units a --fresh refresh could
/// not index, whose shards are as absent as a withheld file's, and the
/// units the load found unservable.
template <typename Ask>
int run_query(const QueryOptions& opts, bool with_build, const char* self_path, Ask ask) {
    auto spelling = opts.where.root();
    auto config = Config::load_from_workspace(CanonicalPath(spelling));
    auto configuration = opts.where.configuration.tag();
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        return fail(std::move(checked.error()));
    }
    std::vector<std::string> failed;
    if(opts.fresh.fresh) {
        auto refreshed = refresh(spelling, config, configuration, self_path);
        if(!refreshed) {
            return fail(refreshed.error());
        }
        failed = std::move(*refreshed);
    }
    FileTable files;
    // Answers name files under the workspace as the command line does.
    files.spell_root(spelling);
    Project project{files};
    CommandResolver commands{project};
    ContextChoices saved;
    EditorContext contexts{project, commands, saved};
    llvm::SmallVector<Fid> dropped;
    bool opened = false;
    if(with_build) {
        if(auto loaded = load_index(project,
                                    commands,
                                    std::move(config),
                                    configuration,
                                    /*with_build=*/true)) {
            dropped = std::move(loaded->dropped);
            saved.bytes = std::move(loaded->contexts);
            contexts.load();
            opened = true;
        }
    } else {
        opened = open_index(project, std::move(config), configuration);
    }
    if(!opened) {
        return fail(
            "the index could not be opened (the log above says why); run `clice index` or pass --fresh");
    }

    index::FreshnessGate gate(project.file_table, {.check_disk = true});
    index::IndexQuery index_query(project.index, project.file_table, &gate, nullptr);
    query::Context ctx{.project = project, .contexts = contexts, .query = index_query};
    auto outcome = ask(ctx);

    std::vector<std::string> stale;
    for(auto file: gate.withheld()) {
        stale.emplace_back(project.file_table.display(file));
    }
    stale.insert(stale.end(), ctx.unindexed.begin(), ctx.unindexed.end());
    stale.insert(stale.end(), failed.begin(), failed.end());
    for(auto unit: dropped) {
        stale.emplace_back(project.file_table.display(unit));
    }
    std::ranges::sort(stale);
    auto duplicates = std::ranges::unique(stale);
    stale.erase(duplicates.begin(), duplicates.end());
    if(outcome) {
        print_json(Answer{.result = std::move(*outcome), .stale = std::move(stale)});
        return 0;
    }
    Failure failure{.error = std::move(outcome.error()), .stale = std::move(stale)};
    if(!ctx.candidates.empty()) {
        failure.candidates = std::move(ctx.candidates);
    }
    print_json(failure);
    return 1;
}

/// A question about the file `ask` takes, after the query context.
template <typename Options, typename Ask>
CommandHandler
    file_question(std::string_view usage, bool with_build, const char* self_path, Ask ask) {
    return operation<Options>(usage, "query", Errors::JSON, [=](const Options& opts) {
        auto args = positional(opts.target.inputs.as_optional(), {"<PATH>"});
        if(!args) {
            return usage_error(Errors::JSON, args.error());
        }
        return run_query(opts.query, with_build, self_path, [&](query::Context& ctx) {
            return ask(ctx, inspected_path(ctx.project, args->front()), opts);
        });
    });
}

/// A question about the symbol `ask` takes, after the query context.
template <typename Options, typename Ask>
CommandHandler symbol_question(std::string_view usage, const char* self_path, Ask ask) {
    return operation<Options>(usage, "query", Errors::JSON, [=](const Options& opts) {
        auto args = positional(opts.target.inputs.as_optional(), {"<LOCATOR>"});
        if(!args) {
            return usage_error(Errors::JSON, args.error());
        }
        return run_query(opts.query, /*with_build=*/false, self_path, [&](query::Context& ctx) {
            return index::SymbolQuery::parse(args->front()).and_then([&](auto locator) {
                return ask(ctx, std::move(locator), opts);
            });
        });
    });
}

CommandHandler build(const char* self_path) {
    auto file = [self_path](std::string_view usage, bool with_build, auto ask) {
        return file_question<FileQuestion>(usage, with_build, self_path, ask);
    };
    auto symbol = [self_path](std::string_view usage, auto ask) {
        return symbol_question<SymbolQuestion>(usage, self_path, ask);
    };
    std::vector<Operation> operations;
    operations.push_back({
        .name = "project-files",
        .description = "The build's files",
        .run = operation<ProjectFilesQuestion>(
            "clice query project-files [OPTIONS]",
            "query",
            Errors::JSON,
            [self_path](const ProjectFilesQuestion& opts) {
                return run_query(opts.query,
                                 /*with_build=*/true,
                                 self_path,
                                 [&](query::Context& ctx) {
                                     return query::project_files(ctx, opts.filter.value_or("all"));
                                 });
            }),
    });
    operations.push_back({
        .name = "file-deps",
        .description = "The files a file includes and the files including it",
        .run = file_question<FileDepsQuestion>(
            "clice query file-deps <PATH> [OPTIONS]",
            /*with_build=*/true,
            self_path,
            [](query::Context& ctx, const Spelling& path, const FileDepsQuestion& opts) {
                return query::file_deps(ctx,
                                        path,
                                        opts.direction.value_or("both"),
                                        opts.depth.value_or(1));
            }),
    });
    operations.push_back({
        .name = "impact-analysis",
        .description = "What a change to a file reaches: its includers and the modules among them",
        .run = file("clice query impact-analysis <PATH> [OPTIONS]",
                    /*with_build=*/true,
                    [](query::Context& ctx, const Spelling& path, const FileQuestion&) {
                        return query::impact_analysis(ctx, path);
                    }),
    });
    operations.push_back({
        .name = "counterparts",
        .description = "The files a file pairs with: a header's sources, a source's headers",
        .run = file("clice query counterparts <PATH> [OPTIONS]",
                    /*with_build=*/true,
                    [](query::Context& ctx, const Spelling& path, const FileQuestion&) {
                        return query::counterparts(ctx, path);
                    }),
    });
    operations.push_back({
        .name = "symbol-search",
        .description = "The symbols a locator matches, best first",
        .run = operation<SearchQuestion>(
            "clice query symbol-search [<LOCATOR>] [OPTIONS]",
            "query",
            Errors::JSON,
            [self_path](const SearchQuestion& opts) {
                auto args = positional(opts.locator.as_optional(), {"<LOCATOR>"}, 1);
                if(!args) {
                    return usage_error(Errors::JSON, args.error());
                }
                auto text = args->empty() ? std::string() : args->front();
                auto limit = static_cast<std::size_t>(std::max(opts.limit.value_or(100), 0));
                return run_query(
                    opts.query,
                    /*with_build=*/false,
                    self_path,
                    [&](query::Context& ctx) { return query::symbol_search(ctx, text, limit); });
            }),
    });
    operations.push_back({
        .name = "read-symbol",
        .description = "A symbol's definition as text, with its documentation",
        .run = symbol("clice query read-symbol <LOCATOR> [OPTIONS]",
                      [](query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                          return query::read_symbol(ctx, std::move(locator));
                      }),
    });
    operations.push_back({
        .name = "document-symbols",
        .description = "A file's outline",
        .run = file("clice query document-symbols <PATH> [OPTIONS]",
                    /*with_build=*/false,
                    [](query::Context& ctx, const Spelling& path, const FileQuestion&) {
                        return query::document_symbols(ctx, path);
                    }),
    });
    operations.push_back({
        .name = "definition",
        .description = "Where a symbol is defined, with its documentation",
        .run = symbol("clice query definition <LOCATOR> [OPTIONS]",
                      [](query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                          return query::definition(ctx, std::move(locator));
                      }),
    });
    operations.push_back({
        .name = "references",
        .description = "Every place naming a symbol",
        .run = symbol_question<ReferencesQuestion>(
            "clice query references <LOCATOR> [OPTIONS]",
            self_path,
            [](query::Context& ctx, index::SymbolQuery locator, const ReferencesQuestion& opts) {
                return query::references(ctx,
                                         std::move(locator),
                                         static_cast<bool>(opts.include_declaration));
            }),
    });
    operations.push_back({
        .name = "call-graph",
        .description = "A function's callers and callees",
        .run = symbol_question<CallGraphQuestion>(
            "clice query call-graph <LOCATOR> [OPTIONS]",
            self_path,
            [](query::Context& ctx, index::SymbolQuery locator, const CallGraphQuestion& opts) {
                return query::call_graph(ctx, std::move(locator), opts.direction.value_or("both"));
            }),
    });
    operations.push_back({
        .name = "implementation",
        .description = "The classes deriving from a class, the overrides of a function",
        .run = symbol("clice query implementation <LOCATOR> [OPTIONS]",
                      [](query::Context& ctx, index::SymbolQuery locator, const SymbolQuestion&) {
                          return query::implementation(ctx, std::move(locator));
                      }),
    });
    operations.push_back({
        .name = "type-hierarchy",
        .description = "A type's supertypes and subtypes",
        .run = symbol_question<TypeHierarchyQuestion>(
            "clice query type-hierarchy <LOCATOR> [OPTIONS]",
            self_path,
            [](query::Context& ctx, index::SymbolQuery locator, const TypeHierarchyQuestion& opts) {
                return query::type_hierarchy(ctx,
                                             std::move(locator),
                                             opts.direction.value_or("both"));
            }),
    });
    return with_operations("Ask the persisted index about the workspace",
                           std::move(operations),
                           Errors::JSON);
}

[[maybe_unused]] const bool registered = register_command(
    Command::query,
    {.description = "Ask the persisted index about the workspace", .build = build});

}  // namespace

}  // namespace clice::driver
