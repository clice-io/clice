#include <algorithm>
#include <print>
#include <string>
#include <vector>

#include "driver/driver.h"
#include "index/writer_lock.h"
#include "project/configuration.h"
#include "project/open_index.h"
#include "sched/batch.h"
#include "server/control_client.h"
#include "server/query_commands.h"

#include "kota/ipc/codec/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct QueryOptions {
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
           help =
               "Question to ask: compileCommand, projectFiles, fileDeps, impactAnalysis, "
               "symbolSearch, readSymbol, documentSymbols, definition, references, "
               "callGraph, typeHierarchy",
           required = false)
    <std::string> method;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "File the question is about (relative to the workspace)",
           required = false)
    <std::string> path;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Symbol name", required = false)
    <std::string> name;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Symbol id (the #<hex> of an earlier answer)",
           required = false)
    <std::string> symbol;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "1-based line, with --path, naming the symbol defined there",
           required = false)
    <int> line;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "symbolSearch: text to search",
           required = false)
    <std::string> query;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "symbolSearch: at most this many symbols (default 100)",
           required = false)
    <int> limit;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "symbolSearch: comma-separated symbol kinds to keep (Function, Struct, ...)",
           required = false)
    <std::string> kind;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "projectFiles: all (default), source, header or module",
           required = false)
    <std::string> filter;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "fileDeps: includes, includers or both (default); callGraph: callers, "
               "callees or both; typeHierarchy: supertypes, subtypes or both",
           required = false)
    <std::string> direction;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "fileDeps: levels to follow (default 1, 0 for all)",
           required = false)
    <int> depth;

    DecoFlag(names = {"--include-declaration"},
             help = "references: list the declarations and definition too",
             required = false)
    include_declaration;

    DecoFlag(names = {"--fresh"},
             help =
                 "Reindex the files whose content changed since they were indexed before "
                 "answering, through the running server or a batch run of this command",
             required = false)
    fresh;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off (default: warn)",
           required = false)
    <std::string> log_level;
};

auto make_command() {
    return kota::deco::cli::command<QueryOptions>("clice query [OPTIONS]");
}

constexpr llvm::StringLiteral build_methods[] = {"compileCommand",
                                                 "projectFiles",
                                                 "fileDeps",
                                                 "impactAnalysis"};
constexpr llvm::StringLiteral index_methods[] = {"symbolSearch",
                                                 "readSymbol",
                                                 "documentSymbols",
                                                 "definition",
                                                 "references",
                                                 "callGraph",
                                                 "typeHierarchy"};

/// One answer on stdout: `{"result": ..., "stale": [...]}`, the files
/// whose rows were withheld because their content moved on from the
/// index (or that the index never held) listed so the reader knows what
/// the answer lacks.
template <typename T>
struct Answer {
    T result;
    std::vector<std::string> stale;
};

/// `{"error": "...", "stale": [...]}` on stdout, exit code 1: what kept
/// the question from being answered, and the files withheld on the way —
/// a symbol not found may sit in one of them.
struct Failure {
    std::string error;
    std::vector<std::string> stale;
};

template <typename T>
std::string render_json(const T& value) {
    auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(value);
    return json ? *json : "null";
}

template <typename T>
void print_json(const T& value) {
    std::println("{}", render_json(value));
}

/// The symbol locator the flags spell, as a name query: `--symbol` an id,
/// `--name` a name query narrowed to `--path`, or `--path` and `--line` a
/// place. The path must be a file; one the index has no rows for is noted
/// as unindexed by the command.
std::expected<index::SymbolQuery, std::string> locator_of(const QueryOptions& opts,
                                                          llvm::StringRef absolute) {
    if(opts.line && *opts.line <= 0) {
        return std::unexpected("line must be positive");
    }
    if(opts.path && !llvm::sys::fs::is_regular_file(absolute)) {
        return std::unexpected(std::format("no such file: {}", std::string_view(absolute)));
    }
    if(opts.symbol) {
        auto parsed = index::SymbolQuery::parse(*opts.symbol);
        if(!parsed || !parsed->handle) {
            return std::unexpected(std::format("invalid symbol id: {}", *opts.symbol));
        }
        return std::move(*parsed);
    }
    // The path is a literal, never query text: it joins the parsed query
    // as the filter or the place it stands for.
    if(opts.name) {
        auto parsed = index::SymbolQuery::parse(*opts.name);
        if(!parsed) {
            return std::unexpected(parsed.error());
        }
        if(opts.path) {
            parsed->paths.emplace_back(absolute);
        }
        return std::move(*parsed);
    }
    if(opts.path && opts.line) {
        index::SymbolQuery query;
        query.position = {.path = std::string(absolute), .line = *opts.line};
        return query;
    }
    return std::unexpected("name a symbol with --name, --symbol, or --path and --line");
}

struct Reply {
    std::string json;
    int exit_code = 0;
};

/// Answer the question against the opened index; the argument checks are
/// the commands' own. `failed` are the units a --fresh refresh could not
/// index: their rows are as absent as a withheld file's. `dropped` are
/// the units the load found unservable.
Reply answer(Project& project,
             EditorContext& contexts,
             const QueryOptions& opts,
             llvm::ArrayRef<std::string> failed,
             llvm::ArrayRef<Fid> dropped) {
    index::DiskGate gate(project.project_index, project.file_table);
    index::IndexQuery index_query(project.project_index, project.file_table, &gate, nullptr);
    query::Context ctx{.project = project, .contexts = contexts, .query = index_query};

    auto method = opts.method.value_or("");
    auto path = opts.path.value_or("");
    auto absolute = path.empty() ? std::string() : inspected_path(project, path);
    auto direction = opts.direction.value_or("both");
    auto kind_list = opts.kind.value_or("");
    llvm::SmallVector<llvm::StringRef> kind_refs;
    llvm::StringRef(kind_list).split(kind_refs, ',', -1, /*KeepEmpty=*/false);
    auto kinds = llvm::to_vector(
        llvm::map_range(kind_refs, [](llvm::StringRef kind) { return kind.trim().str(); }));

    Reply reply;
    auto emit = [&](auto outcome) {
        std::vector<std::string> stale;
        for(auto file: gate.withheld()) {
            stale.emplace_back(project.file_table.resolve(file));
        }
        stale.insert(stale.end(), ctx.unindexed.begin(), ctx.unindexed.end());
        stale.insert(stale.end(), failed.begin(), failed.end());
        for(auto unit: dropped) {
            stale.emplace_back(project.file_table.resolve(unit));
        }
        std::ranges::sort(stale);
        auto duplicates = std::ranges::unique(stale);
        stale.erase(duplicates.begin(), duplicates.end());
        if(outcome) {
            reply.json =
                render_json(Answer{.result = std::move(*outcome), .stale = std::move(stale)});
        } else {
            reply.exit_code = 1;
            reply.json = render_json(
                Failure{.error = std::move(outcome.error()), .stale = std::move(stale)});
        }
    };
    // A file named by --path that the index holds no rows for is reported
    // next to the rows the query withheld.
    auto locator = [&]() -> std::expected<index::SymbolQuery, std::string> {
        auto query = locator_of(opts, absolute);
        if(query && opts.path) {
            auto file = project.file_table.intern(absolute);
            if(!project.project_index.shard(file)) {
                ctx.unindexed.emplace_back(absolute);
                return std::unexpected("symbol not found");
            }
        }
        return query;
    };

    if(method == "compileCommand") {
        emit(query::compile_command(ctx, absolute));
    } else if(method == "projectFiles") {
        emit(query::project_files(ctx, opts.filter.value_or("all")));
    } else if(method == "fileDeps") {
        emit(query::file_deps(ctx, absolute, direction, opts.depth.value_or(1)));
    } else if(method == "impactAnalysis") {
        emit(query::impact_analysis(ctx, absolute));
    } else if(method == "symbolSearch") {
        auto limit = opts.limit.value_or(100);
        emit(query::symbol_search(ctx,
                                  opts.query.value_or(""),
                                  static_cast<std::size_t>(std::max(limit, 0)),
                                  kinds));
    } else if(method == "documentSymbols") {
        emit(query::document_symbols(ctx, absolute));
    } else if(auto query = locator(); !query) {
        emit(query::Outcome<int>(std::unexpected(query.error())));
    } else if(method == "readSymbol") {
        emit(query::read_symbol(ctx, std::move(*query)));
    } else if(method == "definition") {
        emit(query::definition(ctx, std::move(*query)));
    } else if(method == "references") {
        emit(
            query::references(ctx, std::move(*query), static_cast<bool>(opts.include_declaration)));
    } else if(method == "callGraph") {
        emit(query::call_graph(ctx, std::move(*query), direction));
    } else if(method == "typeHierarchy") {
        emit(query::type_hierarchy(ctx, std::move(*query), direction));
    }
    return reply;
}

/// Bring the index up to date with the disk before a --fresh answer:
/// through the serving writer when a server holds the lock, else by a
/// batch run of this process. Either sweeps the build under the hash
/// gate, so only units whose inputs changed are recompiled — and an
/// absent index gets built from nothing. Returns the units that failed
/// to index.
std::expected<std::vector<std::string>, std::string> refresh(llvm::StringRef root,
                                                             llvm::StringRef configuration,
                                                             const char* self_path) {
    auto config = Config::load_from_workspace(root);
    if(!check_requested_configuration(config, configuration)) {
        return std::unexpected(
            std::format("unknown configuration '{}'", std::string_view(configuration)));
    }
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::probe_writer(cache_dir);
    switch(writer.state) {
        case index::WriterProbe::State::Server: {
            auto result = control::request_index(writer.endpoint,
                                                 resolve_configuration(config, configuration));
            if(!result) {
                return std::unexpected(result.error());
            }
            return std::move(result->failed);
        }
        case index::WriterProbe::State::Held: {
            return std::unexpected(index::held_writer_message(writer, cache_dir));
        }
        case index::WriterProbe::State::Free: break;
    }
    auto report_progress = [](const BatchProgress& progress) {
        std::println(stderr,
                     "indexing {}/{} units, {} failed",
                     progress.completed,
                     progress.total,
                     progress.failed);
    };
    auto result = run_batch_index({
        .root = root.str(),
        .configuration = configuration.str(),
        .self_path = self_path,
        .on_progress = report_progress,
    });
    if(!result.completed) {
        return std::unexpected(result.interrupted ? "indexing interrupted"
                                                  : "indexing failed; see the log");
    }
    if(result.unsaved) {
        return std::unexpected("part of the index could not be persisted; see the log");
    }
    return std::move(result.failed);
}

int run_query(const QueryOptions& opts, const char* self_path) {
    auto method = opts.method.value_or("");
    bool with_build = llvm::is_contained(build_methods, method);
    if(!with_build && !llvm::is_contained(index_methods, method)) {
        print_json(Failure{.error = method.empty() ? "--method is required"
                                                   : std::format("unknown method '{}'", method)});
        return 1;
    }
    auto root = workspace_root(opts.workspace.value_or(""));
    auto configuration = opts.configuration.value_or("");
    std::vector<std::string> failed;
    if(opts.fresh) {
        auto refreshed = refresh(root, configuration, self_path);
        if(!refreshed) {
            print_json(Failure{.error = refreshed.error()});
            return 1;
        }
        failed = std::move(*refreshed);
    }
    // The build questions walk manifests and contexts, which only the
    // writer's load restores; the index questions bind the tables in
    // place and touch nothing else.
    FileTable files;
    Project project{files};
    CommandResolver commands{project};
    ContextsBlob saved;
    EditorContext contexts{project, commands, saved};
    llvm::SmallVector<Fid> dropped;
    bool opened = false;
    if(with_build) {
        if(auto loaded = load_index(project, commands, root, configuration, /*with_build=*/true)) {
            dropped = std::move(loaded->dropped);
            saved.bytes = std::move(loaded->contexts);
            contexts.load();
            opened = true;
        }
    } else {
        opened = open_index(project, root, configuration);
    }
    if(!opened) {
        print_json(Failure{
            .error =
                "the index could not be opened (the log above says why); run `clice index` or pass --fresh"});
        return 1;
    }
    auto reply = answer(project, contexts, opts, failed, dropped);
    std::println("{}", reply.json);
    return reply.exit_code;
}

}  // namespace

void add_query(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](QueryOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("warn")))
               return;
           logging::stderr_logger("query", logging::options);
           exit_code = run_query(opts, self_path);
       })
        .on_error([](auto err) { print_json(Failure{.error = err.message}); });

    root.add({.name = "query", .description = "Ask the persisted index about the workspace"},
             std::move(cmd));
}

}  // namespace clice::driver
