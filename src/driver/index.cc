module clice;

import :driver.driver;
import :index.project_index;
import :project.indexer;
import :project.project;
import :server.control;
import :support.logging;
import :support.signal;
import :support.text;
import :support.timer;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Index, description = "Index a project ahead of time");

struct Index {
    constexpr static std::string_view usage_line = "clice index [OPTIONS] [<operation>]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    ProjectOptions where;
    JobsOption jobs;
    JsonReportOption report;
    LogLevelOption log;
    cli::Commands<IndexRecords> operation;
};

/// The current pass's progress to the driver's callback; nothing before
/// the first pass has a total.
void report_progress(BatchRun& run, const BatchOptions& options) {
    auto& background = run.project.background_index;
    auto& pass = background.progress();
    if(!options.on_progress || pass.total == 0) {
        return;
    }
    auto failed = ranges::count_if(background.last_attempts(),
                                   [](const auto& entry) { return entry.second.failed; });
    options.on_progress({.completed = pass.completed,
                         .total = pass.total,
                         .failed = static_cast<std::size_t>(failed)});
}

/// A paced report on top of the pass boundaries: one unit can take
/// longer than the pace, and a run must not fall silent while it runs.
async::task<> progress_ticker(BatchRun& run, const BatchOptions& options) {
    constexpr auto pace = chrono::seconds(10);
    while(true) {
        // Time is the feature: the report's pace.
        co_await async::sleep(pace);
        report_progress(run, options);
    }
}

/// Test hook: CLICE_TEST_PAUSE="<units> <file>" parks a run once that many
/// units are indexed, nothing else dispatched, and creates the file, so a
/// test can signal the run at a known point.
struct TestPause {
    std::size_t units = 0;
    std::string file;
    bool reached = false;
};

std::optional<TestPause> test_pause() {
    std::string value;
    if(!sys::get_env(value, "CLICE_TEST_PAUSE")) {
        return std::nullopt;
    }
    auto [count, file] = llvm::StringRef(value).split(' ');
    TestPause pause;
    if(count.getAsInteger(10, pause.units) || pause.units == 0 || file.empty()) {
        LOG_WARN("CLICE_TEST_PAUSE is '<units> <file>', not '{}'", value);
        return std::nullopt;
    }
    pause.file = file.str();
    return pause;
}

async::task<> index_batch(BatchRun& run, const BatchOptions& options, BatchResult& result) {
    ScopedTimer timer;
    auto& project = run.project;
    auto& background = project.background_index;

    // The machine is the run's: every logical core, while memory admits
    // the units.
    auto& batch = options.project;
    bool started =
        start_batch(run, batch, batch.workers != 0 ? batch.workers : sys::parallelism(), "index");
    result.log_dir = run.log_dir;
    if(!started) {
        result.exit_code = 1;
        // A failed later spawn leaves earlier workers and their I/O tasks
        // live; unstopped they keep the batch event loop spinning and the
        // command hangs instead of exiting.
        co_await run.pool.stop();
        co_return;
    }
    auto opened = project.open({
        .configuration = batch.loaded.configuration,
        .cache = Project::OpenOptions::Cache::Write,
    });

    // The command's whole product is the persisted index: without a database
    // (cache failed to open, another process holds the index writer lock,
    // or an unreadable header disabled persistence) the run would
    // only warm this process's memory and a rerun would start from
    // nothing — fail instead of pretending.
    if(!project.database) {
        LOG_ERROR("Cannot persist the index at {}: the warning above says why; fix that and rerun",
                  std::string_view(project.settings.project.cache_dir));
        result.exit_code = 1;
        co_await shutdown(run);
        co_return;
    }
    if(opened.members.empty()) {
        LOG_ERROR("Nothing to index: {}", project.why_no_members(batch.root));
        result.exit_code = 1;
        co_await shutdown(run);
        co_return;
    }

    auto progress = background.on_progress_changed.connect([&] {
        if(background.progress().stage != BackgroundIndex::Progress::Stage::Report) {
            report_progress(run, options);
        }
    });
    Signal<>::Connection parked;
    if(auto pause = test_pause()) {
        background.pause_after(pause->units);
        parked = background.on_progress_changed.connect([&background, pause = *pause]() mutable {
            if(!pause.reached && background.progress().completed >= pause.units) {
                pause.reached = true;
                if(auto error = fs::write_file(pause.file, "")) {
                    LOG_WARN("CLICE_TEST_PAUSE cannot create {}: {}", pause.file, error.message());
                }
            }
        });
    }
    background.start(/*errors=*/true);
    Lifetime lifetime(run);
    lifetime.aux.spawn(progress_ticker(run, options));
    co_await async::with_token(background.wait_idle(), lifetime.token());
    // The counts read every shard and symbol record: taken before the final
    // save, damage they meet first is discarded there like any other.
    if(!lifetime.stop_requested) {
        result.shard_count = project.index.shard_count();
        result.shard_bytes = project.index.shard_bytes();
        result.symbol_count = project.index.symbol_count();
        result.damaged = project.index.damaged();
    }
    auto interrupted = co_await lifetime.finish();
    // The shutdown save was the last retry for failed writes; whatever is
    // still dirty never reached disk and a rerun cannot resume from it.
    result.unsaved = project.has_unsaved_state();
    if(interrupted) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.indexed_tus = background.indexed();
    for(auto& [unit, attempt]: background.last_attempts()) {
        auto shown = project.file_table.display(unit);
        if(attempt.too_big) {
            result.too_big.push_back({.unit = std::move(shown), .memory = *attempt.too_big});
        } else if(attempt.failed) {
            result.failed.push_back(std::move(shown));
        } else {
            auto& compiled = result.compiled_with_errors.emplace_back(
                BatchResult::UnitErrors{.unit = std::move(shown), .errors = attempt.errors});
            for(auto& error: compiled.errors) {
                show_path(project.file_table, error.file);
            }
        }
    }
    ranges::sort(result.failed);
    ranges::sort(result.too_big, {}, &BatchResult::TooBig::unit);
    ranges::sort(result.compiled_with_errors, {}, &BatchResult::UnitErrors::unit);
    if(!result.failed.empty() || !result.too_big.empty() || result.unsaved || result.damaged) {
        result.exit_code = 1;
    }
    result.seconds = timer.ms() / 1000.0;
}

int run_indexing(const ProjectOptions& where, std::uint32_t workers, bool json) {
    auto read = read_project(where);
    if(!read) {
        return print_error(read.error());
    }
    auto& [root, loaded] = *read;
    // Progress goes to stderr whatever the log level: a run spends most of
    // its time with nothing else to say, and the per-unit log lines exist
    // only at info level. The batch paces the reports, and a tick with the
    // counts unchanged is the heartbeat of a unit that takes longer than
    // the pace.
    auto started = chrono::steady_clock::now();
    auto report_progress = [&](const BatchProgress& progress) {
        driver::println(stderr,
                        "progress {}/{} units, {} failed, {:.0f}s elapsed",
                        progress.completed,
                        progress.total,
                        progress.failed,
                        chrono::duration<double>(chrono::steady_clock::now() - started).count());
    };
    auto indexed =
        index_project({.root = std::move(root), .loaded = std::move(loaded), .workers = workers},
                      report_progress);
    if(!indexed) {
        LOG_ERROR("{}", indexed.error());
        return 1;
    }
    if(auto* server = std::get_if<ServerIndexed>(&*indexed)) {
        // FIXME: the failed units come in the server's spelling of the
        // project, which differs from this command's when the server or the
        // command reaches the project through a symlink.
        auto& failed = server->result.failed;
        if(json) {
            print_json(server->result);
            return failed.empty() ? 0 : 1;
        }
        driver::println("Indexed through the running clice server (pid {}).", server->pid);
        if(!failed.empty()) {
            driver::println(
                "{} translation unit{} failed to index (see the server log); the index is "
                "partial:",
                failed.size(),
                plural_s(failed.size()));
            for(auto& path: failed) {
                driver::println("  {}", path);
            }
            return 1;
        }
        return 0;
    }
    auto& result = std::get<BatchResult>(*indexed);
    if(json) {
        print_json(result);
        return result.exit_code;
    }
    if(result.interrupted) {
        if(result.unsaved) {
            driver::println(
                "Indexing interrupted; part of the progress could not be persisted "
                "(see the log). Rerun `clice index` to resume.");
        } else {
            driver::println("Indexing interrupted; progress saved. Rerun `clice index` to resume.");
        }
        return result.exit_code;
    }
    if(!result.completed) {
        if(!result.log_dir.empty()) {
            driver::println("Session log: {}", result.log_dir);
        }
        return result.exit_code;
    }
    if(result.damaged) {
        driver::println(
            "Indexed {} translation unit{} in {:.1f}s, but the index database is damaged (see the "
            "log): it is discarded, and the next `clice index` rebuilds the index.",
            result.indexed_tus,
            plural_s(result.indexed_tus),
            result.seconds);
    } else {
        driver::println(
            "Indexed {} translation unit{} in {:.1f}s: {} file shard{} ({}), {} symbol{}.",
            result.indexed_tus,
            plural_s(result.indexed_tus),
            result.seconds,
            result.shard_count,
            plural_s(result.shard_count),
            format_bytes(result.shard_bytes),
            result.symbol_count,
            plural_s(result.symbol_count));
    }
    if(!result.compiled_with_errors.empty()) {
        driver::println("{} translation unit{} compiled with errors; the index holds what parsed:",
                        result.compiled_with_errors.size(),
                        plural_s(result.compiled_with_errors.size()));
        for(auto& [unit, errors]: result.compiled_with_errors) {
            auto& first = errors.front();
            driver::println("  {}", unit);
            driver::println("    {}{}",
                            location(first.file, first.line, first.column),
                            first.message);
            if(errors.size() > 1) {
                driver::println("    and {} more error{}",
                                errors.size() - 1,
                                plural_s(errors.size() - 1));
            }
        }
    }
    if(!result.failed.empty()) {
        driver::println(
            "{} translation unit{} failed to index (see the log); the index is partial:",
            result.failed.size(),
            plural_s(result.failed.size()));
        for(auto& path: result.failed) {
            driver::println("  {}", path);
        }
    }
    if(!result.too_big.empty()) {
        driver::println(
            "{} translation unit{} did not fit in memory, even run alone; the index is partial:",
            result.too_big.size(),
            plural_s(result.too_big.size()));
        for(auto& [unit, memory]: result.too_big) {
            driver::println("  {} (reached {})", unit, format_bytes(memory));
        }
    }
    if(result.unsaved) {
        driver::println("Part of the index could not be persisted (see the log).");
    }
    if(!result.log_dir.empty()) {
        driver::println("Session log: {}", result.log_dir);
    }
    return result.exit_code;
}

int run(Index& index) {
    return run_indexing(index.where, index.jobs.jobs, index.report.json);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Index>);

}  // namespace

BatchResult run_batch_index(const BatchOptions& options) {
    async::event_loop loop;
    BatchRun run(loop);
    BatchResult result;
    loop.schedule(index_batch(run, options, result));
    loop.run();
    return result;
}

}  // namespace clice::driver
