module clice;

import :compile.tidy;
import :index.project_index;
import :index.tu_index;
import :project.command;
import :project.graph.pch;
import :project.graph.pcm;
import :project.graph.unit;
import :project.project;
import :support.bytes;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.file_table;
import :worker.protocol;

namespace clice {

namespace {

async::task<UnitResult>
    compile(Project& project, Attempt& attempt, Fid path_id, UnitRequest request) {
    using Status = UnitResult::Status;
    auto file_path = std::string(project.file_table.resolve(path_id));

    worker::CompileUnitParams params;
    params.file = file_path;
    params.project_root = project.root.str();
    params.index = request.index;
    params.errors = request.errors;
    params.tidy = std::move(request.tidy);
    // Units stick to the build's units and their commands: a
    // header, compiled alone under a borrowed command, would fill the
    // index (and the lint report) with guesses, and the units reading it
    // index it as their compile sees it. A file the build dropped whose
    // manifest survives keeps serving its last-known index, and one other
    // units read — by the scan or by their indexed compiles — is indexed
    // by theirs: skipping either loses nothing. One with neither (dropped
    // or never built, read by nothing) stays uncovered — count that as a
    // failure so a batch run reports the missing index instead of exiting
    // clean.
    if(!project.is_unit(path_id)) {
        bool read = !project.dep_graph.find_readers(path_id).empty();
        project.index.for_each_contributor(path_id, [&](Fid) { read = true; });
        if(!project.index.indexed(path_id) && !read) {
            co_return UnitResult{.status = Status::Failed,
                                 .error = "no compile command found; the file stays uncovered"};
        }
        co_return UnitResult{.status = Status::Skipped};
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
    auto resolved = project.resolve_command(path_id, extra);
    params.directory = resolved.directory;
    params.arguments = resolved.arguments;
    params.hidden = resolved.hidden;
    // Taken with the command, not at the merge: a command changing during
    // the build leaves the unit's index recorded under the old one, and the
    // unit owed.
    auto command = project.command_identity(path_id);

    // A unit waits on the PCMs of its imports: the PCMs handed to the worker
    // below would otherwise race a cold build and parse without the module
    // files. The scan runs under the command resolved above, extra args
    // included — a borrowed header host's flags select the same imports the
    // parse will see. A failed PCM build is not terminal — the parse
    // consumes whatever arrived and the worker reports its own failure if
    // they are not enough.
    auto scanned = co_await project.scan_imports(path_id, resolved, std::nullopt);
    // The scan's imports are the unit's edges even when its compile or a
    // build fails, so fixing or providing an import re-dirties it — the
    // include reverse map carries no import edges, and the index records
    // only imports that resolved (ProjectIndex::importers); an unresolved
    // name's edge is what its first provider re-dirties it through.
    project.graph.set_edges(attempt.node(), scanned.declared);
    auto modules = scanned.resolved();
    if(co_await project.wait_modules(&attempt, modules) == Result::Stale) {
        co_return UnitResult{.status = Status::Preempted};
    }
    // Looked up anew after every suspension: another unit's compile may
    // have grown the table meanwhile.
    auto history = [&]() -> UnitHistory& {
        return project.unit_history[path_id];
    };

    // A unit of a target with precompiled headers parses over clice's own
    // build of them: the header its command forces in then reads nothing
    // again, and their declarations are indexed once, with the unit
    // building them. One that did not build leaves that header to every
    // unit's own parse, as the command alone does; so does one whose writing
    // unit a rule keeps out of the index, but for a modular unit
    // (Imports::modular).
    Inputs precompiled;
    std::string consumed_pch;
    std::string left_out_pch;
    if(auto unit = project.pch_writer(path_id, resolved, extra);
       unit &&
       (scanned.modular() || project.rules.indexable(project.file_table.resolve(unit->file)))) {
        auto request = project.declared_pch_request(*unit);
        auto key = request.pch_key;
        if(history().crashed_over_pch == key) {
            left_out_pch = key;
        } else {
            switch(co_await attempt.depend(pch_node(key), project.pch_work(std::move(request)))) {
                case Result::Success:
                    if(auto* pch = project.built_pch(key);
                       pch && !project.pch_reads(key, path_id)) {
                        params.pch = pch->path;
                        precompiled = pch->inputs;
                        consumed_pch = key;
                    }
                    break;
                case Result::Failed: break;
                case Result::Stale: co_return UnitResult{.status = Status::Preempted};
            }
        }
    }

    params.pcms = project.imported_pcms(modules);
    // The modules and the precompiled header the parse reads, taken with
    // their paths: one rebuilt while the parse runs is no input of it.
    auto imports = project.module_inputs(modules);
    merge_inputs(imports, {&precompiled});

    RequestCost cost;
    if(history().cost) {
        cost = *history().cost;
    } else if(auto manifest = project.index.manifest(path_id)) {
        cost.peak = manifest->peak;
    }

    // Merges the unit's index into `result`; its run reached `peak`.
    auto merge = [&](llvm::StringRef index_bytes, std::uint64_t peak, UnitResult result) {
        // A newer content-level invalidation during this build (or a
        // removal clearing the entry) means this result describes text
        // that no longer exists. Drop the merge; the follow-up attempt
        // redoes it.
        if(request.outdated && request.outdated()) {
            LOG_INFO("Discarding outdated index result for {}", file_path);
            return UnitResult{.status = Status::Skipped};
        }
        ScopedTimer merge_timer;
        auto report = project.index.merge(index_bytes, imports, command, peak);
        if(!report) {
            if(report.error() == index::MergeError::Outdated) {
                history().send_in_full = true;
                return UnitResult{.status = Status::Preempted,
                                  .error = "a variant the result named by hash is not stored"};
            }
            // Rejected wholesale: the file's index is missing or stale,
            // which is a failure, not a completed index.
            return UnitResult{.status = Status::Failed,
                              .error = "the TUIndex result failed verification"};
        }
        project.take_report(*report);
        result.perf.merge_ms = merge_timer.ms();
        return result;
    };

    // A module unit's PCM build indexes the unit as its compile does: that
    // index stands for the compile's when the run wants nothing else of it.
    // A unit whose PCM fails to build is left to the compile, which indexes
    // it in spite of its errors.
    if(request.index && !params.tidy && !project.dep_graph.module_of(path_id).empty()) {
        ScopedTimer timer;
        if(co_await project.wait_modules(&attempt, {path_id}) == Result::Stale) {
            co_return UnitResult{.status = Status::Preempted};
        }
        if(auto index = project.module_index(path_id)) {
            auto bytes = index->bytes();
            co_return merge(bytes,
                            cost.peak,
                            {
                                .status = Status::Completed,
                                .perf = {.bytes = bytes.size(), .index_ms = timer.ms()}
            });
        }
    }

    if(request.index && !std::exchange(history().send_in_full, false)) {
        auto include_closure = project.dep_graph.include_closure(path_id);
        params.known_variants = project.index.known_variants(path_id, include_closure);
        params.known_instantiations =
            project.index.known_instantiations(path_id, std::move(include_closure));
    }
    params.standard_library_instantiations =
        project.settings.project.standard_library_instantiations;

    std::optional<CacheStore::PendingEntry> transfer;
    if(request.index && project.store) {
        transfer = project.store->begin_transfer();
        params.index_output_path = transfer->tmp_path;
    }

    StatelessReport measured;
    ScopedTimer timer;
    auto reply = co_await project.pool->send_stateless(params,
                                                       worker::Priority::Low,
                                                       attempt.token(),
                                                       cost,
                                                       &measured);
    // The samples may miss a peak a killed run already reached.
    auto peak = measured.peak != 0 ? std::max(measured.peak, cost.floor) : 0;
    if(peak != 0) {
        history().cost = RequestCost{.peak = peak};
    }
    if(reply.has_value() && reply.value().success) {
        // An overtaken attempt's success clears nothing: what overtook it
        // may be another consumer's failure to read the pair.
        if(!consumed_pch.empty() && attempt.current()) {
            project.pch_consume_succeeded(consumed_pch);
        }
        // The unit parses without the precompiled header it crashed over:
        // that header is what crashed it.
        if(!left_out_pch.empty()) {
            project.pch_consume_failed(left_out_pch);
        }
        history().crashed_over_pch.clear();
        auto run_ms = timer.ms();
        auto& value = reply.value();
        llvm::StringRef index_bytes = as_chars(value.tu_index_data);
        // Declared after `transfer`: the mapping must close before the
        // entry removes the file, which Windows refuses while it is mapped.
        fs::Mapping transferred;
        if(value.index_in_file) {
            auto read = fs::File::open(transfer->tmp_path).and_then(fs::Mapping::map);
            if(!read) {
                co_return UnitResult{.status = Status::Failed,
                                     .error = std::format("reading the index from {} failed: {}",
                                                          transfer->tmp_path,
                                                          read.error().message())};
            }
            transferred = std::move(*read);
            index_bytes = transferred.bytes();
        }
        if(request.index && index_bytes.empty()) {
            co_return UnitResult{.status = Status::Failed,
                                 .error = "the worker returned no TUIndex"};
        }
        UnitResult result;
        result.status = Status::Completed;
        result.tidy_diagnostics = std::move(value.tidy_diagnostics);
        result.errors = std::move(value.errors);
        result.perf = {.bytes = index_bytes.size(), .index_ms = run_ms, .merge_ms = 0};
        if(!request.index) {
            co_return result;
        }
        // A run shorter than a sample interval measured nothing: the
        // earlier peak stands.
        co_return merge(index_bytes, peak != 0 ? peak : cost.peak, std::move(result));
    }

    if(reply.has_value()) {
        history().crashed_over_pch.clear();
        co_return UnitResult{
            .status = Status::Failed,
            .tidy_diagnostics = std::move(reply.value().tidy_diagnostics),
            .error = reply.value().error,
        };
    }
    if(reply.error().code == worker::dispatch_errc::cancelled) {
        co_return UnitResult{.status = Status::Preempted, .error = reply.error().message};
    }
    if(reply.error().code == worker::dispatch_errc::worker_crashed) {
        // The precompiled header it read may be what crashed it, as for a
        // compile (ServedProject::attribute_crash): the next compile parses
        // without it, and only that compile's success counts against it. Discarding
        // the pair at once would fail every unit of the target queued on
        // it, for each unit that crashes on its own code.
        if(!consumed_pch.empty()) {
            history().crashed_over_pch = consumed_pch;
            co_return UnitResult{.status = Status::Preempted,
                                 .error =
                                     "it crashed the worker over its target's precompiled "
                                     "header, which its next compile leaves out: " +
                                     reply.error().message};
        }
        history().crashed_over_pch.clear();
        co_return UnitResult{.status = Status::Crashed,
                             .error = "it crashed the worker: " + reply.error().message};
    }
    if(reply.error().code == worker::dispatch_errc::out_of_memory) {
        auto& data = *reply.error().data;
        auto memory = data["memory"].get_uint().value_or(0);
        auto alone = data["alone"].get_bool().value_or(false);
        history().cost = RequestCost{.peak = cost.peak, .floor = std::max(cost.floor, memory)};
        co_return UnitResult{.status = alone ? Status::TooBig : Status::OutOfMemory,
                             .error = reply.error().message,
                             .memory = memory};
    }
    if(reply.error().code == worker::dispatch_errc::worker_died ||
       reply.error().code == worker::dispatch_errc::worker_lost) {
        co_return UnitResult{.status = Status::Lost, .error = reply.error().message};
    }
    if(reply.error().code == worker::dispatch_errc::worker_unavailable &&
       project.pool->revives_slots()) {
        // The outage is a window, not a decision: the pool revives dead
        // slots, so the requeued attempt can succeed once one returns to
        // service. Without revival the failure below is terminal.
        co_return UnitResult{.status = Status::Preempted, .error = reply.error().message};
    }
    co_return UnitResult{.status = Status::Failed, .error = reply.error().message};
}

/// One attempt, its result kept in `slot` for the waiter.
async::task<Result> attempt_compile(Project& project,
                                    Attempt& attempt,
                                    Fid path_id,
                                    UnitRequest request,
                                    std::shared_ptr<std::optional<UnitResult>> slot) {
    auto& result = slot->emplace(co_await compile(project, attempt, path_id, std::move(request)));
    using enum UnitResult::Status;
    switch(result.status) {
        case Completed: co_return Result::Success;
        case Failed: co_return Result::Failed;
        case Skipped:
        case Crashed:
        case Lost:
        case Preempted:
        case OutOfMemory:
        case TooBig: co_return Result::Stale;
    }
    std::unreachable();
}

}  // namespace

async::task<std::optional<UnitResult>> Project::compile_unit(Fid path_id, UnitRequest request) {
    NodeId id{NodeKind::Unit, path_id.raw};
    // Unit nodes have no dependents: the invalidation cascades nowhere.
    graph.invalidate(id);
    auto slot = std::make_shared<std::optional<UnitResult>>();
    Work work{.run = [this, path_id, request = std::move(request), slot](Attempt& attempt) {
        return attempt_compile(*this, attempt, path_id, request, slot);
    }};
    // An attempt of an earlier request still winding down is waited out:
    // this one's compile follows it.
    co_await graph.join(id, std::move(work), {.retry = [slot] { return !*slot; }});
    co_return std::move(*slot);
}

}  // namespace clice
