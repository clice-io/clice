module clice;

import :compile.tidy;
import :index.project_index;
import :project.background;
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
    // The editor shows them for a file of its own database entry only
    // (ServedProject::shown_skipped_diagnostics).
    params.skipped_diagnostics =
        request.skipped_diagnostics && resolved.ref.source == CommandSource::Database;

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

    // A build compiling the unit indexes it as the run's compile would:
    // when that left the unit's index current, the run wants nothing else
    // of the compile. One that failed leaves the unit to the compile, which
    // indexes it in spite of its errors.
    if(request.index && !params.tidy && !params.skipped_diagnostics &&
       project.compiled_by_build(path_id)) {
        Result built;
        if(!project.dep_graph.module_of(path_id).empty()) {
            built = co_await project.wait_modules(&attempt, {path_id});
        } else {
            auto pch = project.declared_pch_request(resolved.ref);
            auto id = pch_node(pch.pch_key);
            built = co_await attempt.depend(id, project.pch_work(std::move(pch)));
        }
        if(built == Result::Stale) {
            co_return UnitResult{.status = Status::Preempted};
        }
        if(built == Result::Success && project.index_holds(path_id)) {
            co_return UnitResult{.status = Status::Completed};
        }
    }

    // Looked up anew after every suspension: another unit's compile may
    // have grown the table meanwhile.
    auto history = [&]() -> UnitHistory& {
        return project.unit_history[path_id];
    };

    // A unit of a target with precompiled headers parses over clice's own
    // build of them: the header its command forces in then reads nothing
    // again, and their declarations are indexed once, by the build — the
    // writing unit's compile. One that did not build leaves that header to
    // every unit's own parse, as the command alone does; so does one the
    // unit's rules build otherwise than the writing unit's, which no unit
    // compiles, but for a modular unit (Imports::modular).
    Inputs precompiled;
    std::string consumed_pch;
    std::string left_out_pch;
    std::optional<PCHRequest> declared;
    if(auto unit = project.pch_writer(path_id, resolved, extra)) {
        declared = project.declared_pch_request(*unit);
        if(!declared->writer && !scanned.modular()) {
            declared.reset();
        }
    }
    if(declared) {
        auto key = declared->pch_key;
        if(history().crashed_over_pch == key) {
            left_out_pch = key;
        } else {
            switch(co_await attempt.depend(pch_node(key), project.pch_work(std::move(*declared)))) {
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
    IndexTransfer transfer;
    if(request.index) {
        transfer = project.request_index(path_id, params.indexing);
    }

    RequestCost cost;
    if(history().cost) {
        cost = *history().cost;
    } else if(auto manifest = project.index.manifest(path_id)) {
        cost.peak = manifest->peak;
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
        auto index_bytes = transfer.receive(value.tu_index_data, value.index_in_file);
        if(!index_bytes) {
            co_return UnitResult{.status = Status::Failed, .error = std::move(index_bytes.error())};
        }
        if(request.index && index_bytes->empty()) {
            co_return UnitResult{.status = Status::Failed,
                                 .error = "the worker returned no TUIndex"};
        }
        UnitResult result;
        result.status = Status::Completed;
        result.tidy_diagnostics = std::move(value.tidy_diagnostics);
        result.errors = std::move(value.errors);
        result.skipped_diagnostics = std::move(value.skipped_diagnostics);
        result.text_hash = value.text_hash;
        result.perf = {.bytes = index_bytes->size(), .index_ms = run_ms, .merge_ms = 0};
        if(request.index) {
            // A newer content-level invalidation during this build (or a
            // removal clearing the entry) means this result describes text
            // that no longer exists. Drop the merge; the follow-up attempt
            // redoes it.
            if(request.outdated && request.outdated()) {
                LOG_INFO("Discarding outdated index result for {}", file_path);
                co_return UnitResult{.status = Status::Skipped};
            }
            ScopedTimer merge_timer;
            // A run shorter than a sample interval measured nothing: the
            // earlier peak stands.
            auto merged = project.merge_index(path_id,
                                              *index_bytes,
                                              imports,
                                              command,
                                              peak != 0 ? peak : cost.peak);
            if(!merged) {
                if(merged.error() == index::MergeError::Outdated) {
                    co_return UnitResult{.status = Status::Preempted,
                                         .error =
                                             "a variant the result named by hash is not stored"};
                }
                // Rejected wholesale: the file's index is missing or stale,
                // which is a failure, not a completed index.
                co_return UnitResult{.status = Status::Failed,
                                     .error = "the TUIndex result failed verification"};
            }
            result.perf.merge_ms = merge_timer.ms();
        }
        co_return result;
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

bool Project::compiled_by_build(Fid unit) {
    return !dep_graph.module_of(unit).empty() ||
           commands.pch_output(select_command(unit).ref.command);
}

CompiledBy Project::compiled_by(Fid unit) {
    if(background_index.left_to_document(unit)) {
        return CompiledBy::Document;
    }
    return compiled_by_build(unit) ? CompiledBy::Build : CompiledBy::Itself;
}

bool Project::index_holds(Fid unit) {
    return index.indexed_command(unit) == command_identity(unit) && !index.needs_reindex(unit);
}

std::expected<llvm::StringRef, std::string> IndexTransfer::receive(std::span<const std::byte> data,
                                                                   bool in_file) {
    if(!in_file) {
        return as_chars(data);
    }
    auto read = fs::File::open(file->tmp_path).and_then(fs::Mapping::map);
    if(!read) {
        return std::unexpected(std::format("reading the index from {} failed: {}",
                                           file->tmp_path,
                                           read.error().message()));
    }
    mapping = std::move(*read);
    return mapping.bytes();
}

IndexTransfer Project::request_index(Fid unit, worker::IndexRequest& request) {
    if(!std::exchange(unit_history[unit].send_in_full, false)) {
        auto include_closure = dep_graph.include_closure(unit);
        request.known_variants = index.known_variants(unit, include_closure);
        request.known_instantiations = index.known_instantiations(unit, std::move(include_closure));
    }
    request.standard_library_instantiations = settings.project.standard_library_instantiations;
    IndexTransfer transfer;
    if(store) {
        transfer.file = store->begin_transfer();
        request.output_path = transfer.file->tmp_path;
    }
    return transfer;
}

std::expected<void, index::MergeError> Project::merge_index(Fid unit,
                                                            llvm::StringRef tu_index,
                                                            llvm::ArrayRef<Input> imports,
                                                            std::uint64_t command,
                                                            std::uint64_t peak) {
    auto report = index.merge(tu_index, imports, command, peak);
    if(!report) {
        if(report.error() == index::MergeError::Outdated) {
            unit_history[unit].send_in_full = true;
        }
        return std::unexpected(report.error());
    }
    take_report(*report);
    return {};
}

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
