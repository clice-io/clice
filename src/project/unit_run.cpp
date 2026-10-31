module clice;

import :compile.tidy;
import :index.project_index;
import :project.command;
import :project.pch;
import :project.pcm;
import :project.project;
import :project.unit_run;
import :support.bytes;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.file_table;
import :worker.protocol;

namespace clice {

namespace {

NodeId node(Fid path_id) {
    return {NodeKind::UnitRun, path_id.raw};
}

}  // namespace

async::task<UnitRunOutcome> run_unit(Project& project, Fid path_id, UnitRunPlan plan) {
    project.unit_run_plans[path_id] = std::move(plan);
    // A clean node left by an earlier success must not satisfy this request
    // without running: the request's existence means work is owed, so
    // re-mark the node dirty. Unit-run nodes have no dependents — the
    // update cascades nowhere.
    project.graph.update(node(path_id));
    auto join = co_await project.graph.request(node(path_id), {.kind = JoinKind::OneAttempt});
    project.unit_run_plans.erase(path_id);
    auto it = project.unit_run_outcomes.find(path_id);
    if(it == project.unit_run_outcomes.end()) {
        assert(join == JoinOutcome::Shutdown && "a finished unit run records its outcome");
        co_return UnitRunOutcome{};
    }
    auto outcome = std::move(it->second);
    project.unit_run_outcomes.erase(it);
    co_return outcome;
}

async::task<RoundOutcome> unit_run_round(Project& project, RoundContext& ctx, NodeId id) {
    using Result = UnitRunOutcome::Result;
    auto path_id = id.file();
    auto& finished = project.unit_run_outcomes;
    auto it = project.unit_run_plans.find(path_id);
    assert(it != project.unit_run_plans.end() && "a unit run's round spawns only under run_unit");
    // Copied: the map may rehash under a concurrent run_unit for another
    // file while this round is suspended.
    auto plan = it->second;

    auto file_path = std::string(project.file_table.resolve(path_id));

    worker::UnitRunParams params;
    params.file = file_path;
    params.project_root = project.root.str();
    params.index = plan.index;
    params.errors = plan.errors;
    params.tidy = std::move(plan.tidy);
    // Whole-TU runs stick to the build's units and their commands: a
    // header, compiled alone under a borrowed command, would fill the
    // index (and the lint report) with guesses, and the units reading it
    // index it as their compile sees it. A file the build dropped whose
    // manifest survives keeps serving its last-known index, and one other
    // units read — by the scan or by their indexed compiles — is indexed
    // by theirs: skipping either loses nothing. One with neither (dropped
    // or never built, read by nothing) stays uncovered — count that as a
    // failure so a batch run reports the missing index instead of exiting
    // clean.
    if(!project.build.unit(path_id)) {
        bool read = !project.dep_graph.find_readers(path_id).empty();
        project.index.for_each_contributor(path_id, [&](Fid) { read = true; });
        if(!project.index.indexed(path_id) && !read) {
            finished[path_id] = {.result = Result::Failed,
                                 .error = "no compile command found; the file stays uncovered"};
            co_return RoundOutcome::Failed;
        }
        finished[path_id] = {.result = Result::Skipped};
        co_return RoundOutcome::Stale;
    }
    // A lint plan's extra args join the driver command here, before
    // toolchain resolution — the driver interprets them (pass-throughs,
    // --target) when it produces the resolved line, and every later
    // consumer of params.arguments (dependency scan, worker parse) sees
    // one truth.
    tidy::CommandExtraArgs extras;
    if(params.tidy) {
        extras = tidy::command_extra_args(params.tidy->extra_args, params.tidy->extra_args_before);
    }
    CommandRequest extra{.extra_prepend = extras.prepend, .extra_append = extras.append};
    auto resolved = resolve_command(project, path_id, params.directory, params.arguments, extra);
    params.hidden = resolved.hidden;
    // The editor shows them for a file of its own database entry only
    // (ServedProject::shown_skipped_diagnostics).
    params.skipped_diagnostics =
        plan.skipped_diagnostics && resolved.ref.source == CommandSource::CDBExact;

    // A unit waits on the PCMs of its imports: the fill_pcm_deps snapshot
    // below would otherwise race a cold build and parse without the module
    // files. The scan runs under the command resolved above, extra args
    // included — a borrowed header host's flags select the same imports the
    // parse will see. A failed PCM build is not terminal — the parse
    // consumes whatever arrived and the worker reports its own failure if
    // they are not enough.
    std::vector<const char*> argv;
    argv.reserve(params.arguments.size());
    for(auto& arg: params.arguments) {
        argv.push_back(arg.c_str());
    }
    auto deps =
        co_await direct_deps(project, path_id, resolved, argv, params.directory, std::nullopt);
    declare_imports(project, ctx, id, deps);

    // On-disk PCM blobs can be LRU-evicted while their nodes stay clean;
    // re-dirty evicted ones so depend() rebuilds instead of handing the
    // worker a dead path. Bounded: a rebuild can itself evict under budget
    // pressure, and past the bound the parse fails visibly on the missing
    // file.
    auto modules = deps.resolved();
    for(int attempt = 0; !modules.empty() && attempt < 3; attempt += 1) {
        bool any_evicted = revalidate_pcms(project);
        if(attempt > 0 && !any_evicted) {
            break;
        }
        for(auto dep: modules) {
            if(co_await ctx.depend({NodeKind::PCM, dep.raw}) == DependResult::Cancelled) {
                finished[path_id] = {.result = Result::Preempted};
                co_return RoundOutcome::Stale;
            }
        }
    }

    // A unit of a target with precompiled headers parses over clice's own
    // build of them: the header its command forces in then reads nothing
    // again, and their declarations are indexed once, with the unit
    // building them. One that did not build leaves that header to every
    // unit's own parse, as the command alone does; a builder left out of
    // the index does too, but for a modular unit (ModuleDeps::modular).
    DepsSnapshot precompiled;
    std::string consumed_pch;
    std::string left_out_pch;
    if(auto unit = pch_unit(project, path_id, resolved, extra);
       unit && (deps.modular() || project.build.indexed(project.file_table.resolve(unit->file)))) {
        auto request = declared_pch_request(project, *unit, /*skip_bodies=*/false);
        auto key = request.pch_key;
        if(auto crashed = project.crashed_over_pch.find(path_id);
           crashed != project.crashed_over_pch.end() && crashed->second == key) {
            left_out_pch = key;
        } else {
            switch(co_await ctx.depend(prepare_pch(project, std::move(request)))) {
                case DependResult::Ready:
                    if(auto it = project.pch_cache.find(key); it != project.pch_cache.end() &&
                                                              !it->second.path.empty() &&
                                                              !pch_reads(project, key, path_id)) {
                        params.pch = it->second.path;
                        precompiled = it->second.deps;
                        consumed_pch = key;
                    }
                    break;
                case DependResult::Failed: break;
                case DependResult::Cancelled:
                    finished[path_id] = {.result = Result::Preempted};
                    co_return RoundOutcome::Stale;
            }
        }
    }

    project.fill_pcm_deps(params.pcms, module_closure(project, modules).getArrayRef());
    // The modules and the precompiled header the parse reads, taken with
    // their paths: one rebuilt while the parse runs is no input of it.
    auto imports = project.module_inputs(modules);
    merge_deps(imports, {&precompiled});
    if(plan.index && !project.send_in_full.erase(path_id)) {
        auto include_closure = project.dep_graph.include_closure(path_id);
        params.known_variants = project.index.known_variants(path_id, include_closure);
        params.known_instantiations =
            project.index.known_instantiations(path_id, std::move(include_closure));
    }
    params.standard_library_instantiations =
        project.settings.project.standard_library_instantiations;

    std::optional<CacheStore::PendingEntry> transfer;
    if(plan.index && project.store) {
        transfer = project.store->begin_transfer();
        params.index_output_path = transfer->tmp_path;
    }

    auto& costs = project.unit_run_costs;
    RequestCost cost;
    if(auto it = costs.find(path_id); it != costs.end()) {
        cost = it->second;
    } else if(auto manifest = project.index.manifest(path_id)) {
        cost.peak = manifest->peak;
    }
    StatelessReport measured;
    ScopedTimer timer;
    auto reply = co_await project.pool->send_stateless(params,
                                                       worker::Priority::Low,
                                                       ctx.token(),
                                                       cost,
                                                       &measured);
    // The samples may miss a peak a killed run already reached.
    auto peak = measured.peak != 0 ? std::max(measured.peak, cost.floor) : 0;
    if(peak != 0) {
        costs[path_id] = {.peak = peak};
    }
    if(reply.has_value() && reply.value().success) {
        // A voided round's success clears nothing: the void may be another
        // consumer's failure to read the pair.
        if(!consumed_pch.empty() && ctx.current()) {
            pch_consume_succeeded(project, consumed_pch);
        }
        // The unit parses without the precompiled header it crashed over:
        // that header is what crashed it.
        if(!left_out_pch.empty()) {
            pch_consume_failed(project, left_out_pch);
        }
        project.crashed_over_pch.erase(path_id);
        auto run_ms = timer.ms();
        auto& value = reply.value();
        llvm::StringRef index_bytes = as_chars(value.tu_index_data);
        // Declared after `transfer`: the mapping must close before the
        // entry removes the file, which Windows refuses while it is mapped.
        fs::Mapping transferred;
        if(value.index_in_file) {
            auto read = fs::File::open(transfer->tmp_path).and_then(fs::Mapping::map);
            if(!read) {
                finished[path_id] = {.result = Result::Failed,
                                     .error = std::format("reading the index from {} failed: {}",
                                                          transfer->tmp_path,
                                                          read.error().message())};
                co_return RoundOutcome::Failed;
            }
            transferred = std::move(*read);
            index_bytes = transferred.bytes();
        }
        if(plan.index && index_bytes.empty()) {
            finished[path_id] = {.result = Result::Failed,
                                 .error = "the worker returned no TUIndex"};
            co_return RoundOutcome::Failed;
        }
        UnitRunOutcome outcome;
        outcome.result = Result::Completed;
        outcome.tidy_diagnostics = std::move(value.tidy_diagnostics);
        outcome.errors = std::move(value.errors);
        outcome.skipped_diagnostics = std::move(value.skipped_diagnostics);
        outcome.text_hash = value.text_hash;
        outcome.perf = {.bytes = index_bytes.size(), .index_ms = run_ms, .merge_ms = 0};
        if(plan.index) {
            // A newer content-level invalidation during this build (or a
            // removal clearing the entry) means this result describes text
            // that no longer exists. Drop the merge; the follow-up slot
            // redoes it.
            if(plan.outdated && plan.outdated()) {
                LOG_INFO("Discarding outdated index result for {}", file_path);
                finished[path_id] = {.result = Result::Skipped};
                co_return RoundOutcome::Stale;
            }
            ScopedTimer merge_timer;
            // A run shorter than a sample interval measured nothing: the
            // earlier peak stands.
            auto report = project.index.merge(index_bytes, imports, peak != 0 ? peak : cost.peak);
            if(!report) {
                if(report.error() == index::MergeError::Outdated) {
                    project.send_in_full.insert(path_id);
                    finished[path_id] = {.result = Result::Preempted,
                                         .error =
                                             "a variant the result named by hash is not stored"};
                    co_return RoundOutcome::Stale;
                }
                // Rejected wholesale: the file's index is missing or stale,
                // which is a failure, not a completed index.
                finished[path_id] = {.result = Result::Failed,
                                     .error = "the TUIndex result failed verification"};
                co_return RoundOutcome::Failed;
            }
            outcome.report = std::move(*report);
            outcome.perf.merge_ms = merge_timer.ms();
        }
        finished[path_id] = std::move(outcome);
        co_return RoundOutcome::Success;
    }

    if(reply.has_value()) {
        project.crashed_over_pch.erase(path_id);
        finished[path_id] = {
            .result = Result::Failed,
            .tidy_diagnostics = std::move(reply.value().tidy_diagnostics),
            .error = reply.value().error,
        };
        co_return RoundOutcome::Failed;
    }
    if(reply.error().code == worker::dispatch_errc::cancelled) {
        finished[path_id] = {.result = Result::Preempted, .error = reply.error().message};
        co_return RoundOutcome::Stale;
    }
    if(reply.error().code == worker::dispatch_errc::worker_crashed) {
        // The precompiled header it read may be what crashed it, as for a
        // compile (ServedProject::attribute_crash): the next run parses without
        // it, and only that run's success counts against it. Discarding
        // the pair at once would fail every unit of the target queued on
        // it, for each unit that crashes on its own code.
        if(!consumed_pch.empty()) {
            project.crashed_over_pch[path_id] = consumed_pch;
            finished[path_id] = {.result = Result::Preempted,
                                 .error =
                                     "it crashed the worker over its target's precompiled "
                                     "header, which its next run leaves out: " +
                                     reply.error().message};
            co_return RoundOutcome::Stale;
        }
        project.crashed_over_pch.erase(path_id);
        finished[path_id] = {.result = Result::Crashed,
                             .error = "it crashed the worker: " + reply.error().message};
        co_return RoundOutcome::Stale;
    }
    if(reply.error().code == worker::dispatch_errc::out_of_memory) {
        auto& data = *reply.error().data;
        auto memory = data["memory"].get_uint().value_or(0);
        auto alone = data["alone"].get_bool().value_or(false);
        costs[path_id] = {.peak = cost.peak, .floor = std::max(cost.floor, memory)};
        finished[path_id] = {.result = alone ? Result::TooBig : Result::OutOfMemory,
                             .error = reply.error().message,
                             .memory = memory};
        co_return RoundOutcome::Stale;
    }
    if(reply.error().code == worker::dispatch_errc::worker_died ||
       reply.error().code == worker::dispatch_errc::worker_lost) {
        finished[path_id] = {.result = Result::Lost, .error = reply.error().message};
        co_return RoundOutcome::Stale;
    }
    if(reply.error().code == worker::dispatch_errc::worker_unavailable &&
       project.pool->revives_slots()) {
        // The outage is a window, not a decision: the pool revives dead
        // slots, so the requeued attempt can succeed once one returns to
        // service. Without revival the failure below is terminal.
        finished[path_id] = {.result = Result::Preempted, .error = reply.error().message};
        co_return RoundOutcome::Stale;
    }
    finished[path_id] = {.result = Result::Failed, .error = reply.error().message};
    co_return RoundOutcome::Failed;
}

}  // namespace clice
