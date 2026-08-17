#include <csignal>
#include <format>
#include <print>
#include <span>

#include "driver/driver.h"
#include "server/transport/master_server.h"
#include "support/filesystem.h"
#include "support/timer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FileSystem.h"

namespace clice::driver {

namespace {

struct IndexOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "Number of indexing workers (default: from config)",
           required = false)
    <std::uint32_t> workers;

    DecoFlag(names = {"--stats"},
             help = "Print statistics of the persisted index instead of indexing",
             required = false)
    stats;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           help = "How many of the largest file shards --stats lists",
           required = false)
    <std::uint32_t> top = 20;

    DecoKV(style = deco::decl::KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";
};

auto make_command() {
    return kota::deco::cli::command<IndexOptions>("clice index [OPTIONS]");
}

std::string format_size(std::uint64_t bytes) {
    if(bytes >= 1024 * 1024) {
        return std::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
    }
    if(bytes >= 1024) {
        return std::format("{:.1f} KB", bytes / 1024.0);
    }
    return std::format("{} B", bytes);
}

/// Poll until the background indexer has drained every round (requeue
/// rounds included) and persisted its results.
kota::task<> wait_until_indexed(const MasterServer& server) {
    while(!server.indexer.is_idle()) {
        co_await kota::sleep(200);
    }
}

/// The first signal asks for a graceful stop: in-flight files are
/// abandoned, finished ones are persisted, and a rerun resumes from
/// there. A second signal exits immediately.
kota::task<> watch_signal(MasterServer& server, int signum) {
    auto watcher = kota::signal::create();
    if(!watcher || watcher->start(signum).has_error()) {
        co_return;
    }
    co_await watcher->wait();
    LOG_INFO("Interrupted; saving indexing progress");
    server.schedule_shutdown();
    co_await watcher->wait();
    std::_Exit(130);
}

kota::task<> run_indexing_task(MasterServer& server, std::string root, int& exit_code) {
    ScopedTimer timer;
    server.initialize(root);
    if(server.lifecycle != ServerLifecycle::Ready) {
        exit_code = 1;
        co_await server.shutdown_and_cleanup();
        co_return;
    }
    if(server.workspace.cdb.get_entries().empty()) {
        LOG_ERROR("Nothing to index: no compile_commands.json found under {}", root);
        exit_code = 1;
        co_await server.shutdown_and_cleanup();
        co_return;
    }

    kota::task_group<> aux(server.loop);
    aux.spawn(watch_signal(server, SIGINT));
    aux.spawn(watch_signal(server, SIGTERM));

    co_await kota::with_token(wait_until_indexed(server), server.shutdown_token());
    bool interrupted = server.lifecycle == ServerLifecycle::ShuttingDown;
    co_await server.shutdown_and_cleanup();
    aux.cancel();
    co_await aux.join();

    if(interrupted) {
        std::println("Indexing interrupted; progress saved. Rerun `clice index` to resume.");
        exit_code = 130;
        co_return;
    }
    auto& workspace = server.workspace;
    std::uint64_t total_bytes = 0;
    for(auto& shard: llvm::make_second_range(workspace.shards)) {
        total_bytes += shard.bytes().size();
    }
    std::println("Indexed {} translation units in {:.1f}s: {} file shards ({}), {} symbols.",
                 workspace.project_index.manifests.size(),
                 timer.ms() / 1000.0,
                 workspace.shards.size(),
                 format_size(total_bytes),
                 workspace.project_index.symbols.size());
}

int run_indexing(std::string root, std::uint32_t workers, const char* self_path) {
    kota::event_loop loop;
    MasterServer server(loop, self_path);

    // A one-shot batch run: rounds start immediately, disk polling stays
    // off, and indexing happens even when the config keeps the background
    // index disabled — running `clice index` is the request itself.
    std::string worker_overlay;
    if(workers != 0) {
        worker_overlay = std::format(
            R"(, "stateless_worker_count": {0}, "min_stateless_worker_count": {0}, "max_stateless_worker_count": {0})",
            workers);
    }
    server.init_options_json = std::format(
        R"({{"project": {{"idle_timeout_ms": 0, "enable_indexing": true{}}}, "tracker": {{"cdb_poll_seconds": 0, "workspace_poll_seconds": 0}}}})",
        worker_overlay);

    int exit_code = 0;
    loop.schedule(run_indexing_task(server, std::move(root), exit_code));
    loop.run();
    return exit_code;
}

int run_stats(llvm::StringRef root, std::uint32_t top) {
    auto config = Config::load_from_workspace(root);
    if(!llvm::sys::fs::exists(config.project.cache_dir)) {
        LOG_ERROR("No index cache at {}; run `clice index` first",
                  std::string_view(config.project.cache_dir));
        return 1;
    }
    auto store = CacheStore::open(config.project.cache_dir, cache_format_version);
    if(!store) {
        LOG_ERROR("Failed to open cache store at {}: {}",
                  std::string_view(config.project.cache_dir),
                  store.error().message());
        return 1;
    }

    kota::event_loop loop;
    Workspace workspace;
    workspace.config = std::move(config);
    workspace.store.emplace(std::move(*store));
    workspace.index_storage = index::make_fs_index_storage(*workspace.store);
    WorkerPool pool(loop);
    ContextResolver contexts(workspace);
    SessionStore sessions;
    Indexer indexer(loop, workspace, pool, contexts, sessions);
    indexer.load(/*read_only=*/true);

    auto& project = workspace.project_index;
    if(project.manifests.empty() && workspace.shards.empty()) {
        std::println("Index is empty; run `clice index` to build it.");
        return 0;
    }

    struct ShardStat {
        llvm::StringRef path;
        std::uint64_t bytes;
        std::size_t variants;
        std::uint64_t occurrences = 0;
        std::uint64_t relations = 0;
    };

    std::vector<ShardStat> files;
    files.reserve(workspace.shards.size());
    std::uint64_t total_bytes = 0, total_occurrences = 0, total_relations = 0;
    for(auto& [path_id, shard]: workspace.shards) {
        ShardStat stat{workspace.path_pool.resolve(path_id),
                       shard.bytes().size(),
                       shard.variants().size()};
        shard.for_each_occurrence([&](const index::Occurrence&) {
            stat.occurrences += 1;
            return true;
        });
        shard.for_each_relation([&](index::SymbolHash, const index::Relation&) {
            stat.relations += 1;
            return true;
        });
        total_bytes += stat.bytes;
        total_occurrences += stat.occurrences;
        total_relations += stat.relations;
        files.push_back(stat);
    }
    std::ranges::sort(files, std::ranges::greater{}, &ShardStat::bytes);

    std::println("Index cache: {}", std::string_view(workspace.store->base_dir()));
    std::println("Translation units: {}", project.manifests.size());
    std::println("File shards: {} ({}), {} occurrences, {} relations",
                 files.size(),
                 format_size(total_bytes),
                 total_occurrences,
                 total_relations);
    std::println("Global symbols: {}, file versions: {}",
                 project.symbols.size(),
                 project.file_versions.size());
    if(indexer.pending_files() != 0) {
        std::println("Translation units pending reindex (stale or partially written): {}",
                     indexer.pending_files());
    }
    std::println("");
    std::println("Top {} file shards by size:", std::min<std::size_t>(top, files.size()));
    for(auto& stat: std::span(files).subspan(0, std::min<std::size_t>(top, files.size()))) {
        std::println("  {:>10}  {:>4} variants  {:>9} occs  {:>9} rels  {}",
                     format_size(stat.bytes),
                     stat.variants,
                     stat.occurrences,
                     stat.relations,
                     std::string_view(stat.path));
    }
    return 0;
}

}  // namespace

void add_index(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](IndexOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           logging::stderr_logger("index", logging::options);

           llvm::SmallString<256> workspace(opts.workspace.value_or(""));
           if(workspace.empty()) {
               llvm::sys::fs::current_path(workspace);
           } else {
               llvm::sys::fs::make_absolute(workspace);
           }
           std::string ws(workspace.str());
           path::canonicalize(ws);

           if(opts.stats) {
               exit_code = run_stats(ws, opts.top.value_or(20));
               return;
           }
           exit_code = run_indexing(std::move(ws), opts.workers.value_or(0), self_path);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "index", .description = "Index a workspace ahead of time"}, std::move(cmd));
}

}  // namespace clice::driver
