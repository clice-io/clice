module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :compile.compilation;
import :sched.families.pcm;
import :sched.families.turun;
import :support.logging;
import :support.timer;
import :vfs.file_system;
import :worker.protocol;

namespace clice {

TURunFamily::TURunFamily(TaskGraph& graph,
                         Project& project,
                         CommandResolver& commands,
                         PCMFamily& pcm,
                         IndexStore& store,
                         WorkerPool& pool) :
    graph(graph), project(project), commands(commands), pcm(pcm), store(store), pool(pool) {}

void TURunFamily::register_runner() {
    graph.register_family(Family::TURun, [this](RoundContext& ctx, NodeId id) {
        return round(ctx, Fid{static_cast<std::uint32_t>(id.key)});
    });
}

kota::task<TURunFamily::Outcome> TURunFamily::run(Fid path_id, Plan plan, Guards guards) {
    inputs[path_id] = {std::move(plan), std::move(guards)};
    // A clean node left by an earlier success must not satisfy this request
    // without running: the request's existence means work is owed, so
    // re-mark the node dirty. TURun nodes have no dependents — the update
    // cascades nowhere.
    graph.update(node(path_id));
    auto join = co_await graph.request(node(path_id), {.flavor = JoinFlavor::OneAttempt});
    inputs.erase(path_id);
    auto it = landed.find(path_id);
    if(it == landed.end()) {
        assert(join == JoinOutcome::Shutdown && "a landed TURun round records its outcome");
        co_return Outcome{};
    }
    auto outcome = std::move(it->second);
    landed.erase(it);
    co_return outcome;
}

kota::task<RoundOutcome> TURunFamily::round(RoundContext& ctx, Fid path_id) {
    auto it = inputs.find(path_id);
    assert(it != inputs.end() && "a TURun round spawns only under run()'s stash");
    // Copied: the map may rehash under a concurrent run() for another
    // file while this round is suspended.
    auto [plan, guards] = it->second;

    auto file_path = std::string(project.file_table.resolve(path_id));

    worker::TURunParams params;
    params.file = file_path;
    params.workspace = project.config.workspace_root.str();
    params.index = plan.index;
    params.tidy = std::move(plan.tidy);
    // Whole-TU runs stick to the build's commands; a synthesized one, or
    // one borrowed for a file outside the build, would fill the index (and
    // the lint report) with guesses. A lint plan's extra args join the
    // driver command here, before toolchain resolution — the driver
    // interprets them (pass-throughs, --target) when it produces the
    // resolved line, and every later consumer of params.arguments
    // (dependency scan, worker parse) sees one truth.
    tidy::CommandExtraArgs extras;
    if(params.tidy) {
        extras = tidy::command_extra_args(params.tidy->extra_args, params.tidy->extra_args_before);
    }
    auto resolved =
        commands.resolve_command(path_id,
                                 params.directory,
                                 params.arguments,
                                 {.extra_prepend = extras.prepend, .extra_append = extras.append});
    if(resolved.source == CommandSource::Fallback ||
       (resolved.source == CommandSource::Inferred && !project.build.unit(path_id))) {
        // A file whose manifest survives keeps serving its last-known rows,
        // so skipping it loses nothing. One without a manifest (dropped or
        // never built) stays uncovered — count that as a failure so a batch
        // run reports the debt instead of exiting clean.
        if(!project.project_index.manifests.contains(path_id)) {
            landed[path_id] = {.verdict = Verdict::Failed,
                               .error = "no compile command found; the file stays uncovered"};
            co_return RoundOutcome::Failed;
        }
        landed[path_id] = {.verdict = Verdict::Skipped};
        co_return RoundOutcome::Stale;
    }
    if(resolved.synthesized) {
        params.synthesized = resolved.synthesized->files;
    }

    // A unit waits on the PCMs of its imports: the fill_pcm_deps snapshot
    // below would otherwise race a cold build and parse without the module
    // files. The scan runs under the command resolved above, extra args
    // included — a borrowed header host's flags select the same imports the
    // parse will see — and its sentinel edges are what let an unresolved
    // name's first provider re-dirty this TU. A failed PCM build is not
    // terminal — the parse consumes whatever artifacts landed and the
    // worker reports its own failure if they are not enough.
    std::vector<const char*> argv;
    argv.reserve(params.arguments.size());
    for(auto& arg: params.arguments) {
        argv.push_back(arg.c_str());
    }
    auto deps = co_await pcm.direct_deps(path_id, resolved, argv, params.directory, std::nullopt);

    // Scanner truth outlives the run: committed as durable edges even
    // when the run or a build fails, so fixing or providing an import
    // re-dirties this TU — the include reverse map carries no import
    // edges, and the index records only imports that resolved
    // (ProjectIndex::importers).
    graph.declare(node(path_id), deps.declared);
    for(auto dep: deps.declared) {
        if(PCMFamily::is_unresolved(dep)) {
            ctx.reference(dep);
        }
    }

    // On-disk PCM blobs can be LRU-evicted while their nodes stay clean;
    // re-dirty evicted ones so depend() rebuilds instead of handing the
    // worker a dead path. Bounded: a rebuild can itself evict under budget
    // pressure, and past the bound the parse fails visibly on the missing
    // file.
    for(int attempt = 0; !deps.resolved.empty() && attempt < 3; attempt += 1) {
        bool any_evicted = pcm.revalidate_blobs();
        if(attempt > 0 && !any_evicted) {
            break;
        }
        for(auto dep: deps.resolved) {
            if(co_await ctx.depend({Family::PCM, dep.raw}) == DependResult::Cancelled) {
                landed[path_id] = {.verdict = Verdict::Preempted};
                co_return RoundOutcome::Stale;
            }
        }
    }

    project.fill_pcm_deps(params.pcms, path_id);
    // The modules the parse reads, taken with the PCM paths: a module
    // rebuilt while the parse runs is no input of it.
    auto imports = project.module_inputs(deps.resolved);
    if(plan.index && !send_in_full.erase(path_id)) {
        params.known_variants = store.known_variants(path_id);
    }

    std::optional<CacheStore::PendingEntry> transfer;
    if(plan.index && project.store) {
        transfer = project.store->begin_transfer();
        params.index_output_path = transfer->tmp_path;
    }

    ScopedTimer timer;
    auto result = co_await pool.send_stateless(params, worker::Priority::Low, ctx.token());
    if(result.has_value() && result.value().success) {
        auto run_ms = timer.ms();
        auto& value = result.value();
        llvm::StringRef index_bytes = value.tu_index_data;
        // Declared after `transfer`: the mapping must close before the
        // entry removes the file, which Windows refuses while it is mapped.
        std::unique_ptr<llvm::MemoryBuffer> transferred;
        if(value.index_in_file) {
            auto read = vfs::read(transfer->tmp_path, vfs::Read::Mapped);
            if(!read) {
                landed[path_id] = {.verdict = Verdict::Failed,
                                   .error = std::format("reading the index from {} failed: {}",
                                                        transfer->tmp_path,
                                                        read.error().message())};
                co_return RoundOutcome::Failed;
            }
            transferred = std::move(*read);
            index_bytes = transferred->getBuffer();
        }
        if(plan.index && index_bytes.empty()) {
            landed[path_id] = {.verdict = Verdict::Failed,
                               .error = "the worker returned no TUIndex"};
            co_return RoundOutcome::Failed;
        }
        Outcome outcome;
        outcome.verdict = Verdict::Completed;
        outcome.tidy_diagnostics = std::move(value.tidy_diagnostics);
        outcome.perf = {.bytes = index_bytes.size(), .index_ms = run_ms, .merge_ms = 0};
        if(plan.index) {
            // Merge guard: a newer content-level invalidation during this
            // build (or a removal clearing the entry) means this result
            // describes text that no longer exists — e.g. a compile-command
            // change whose erase+re-enqueue must not be undone by an
            // in-flight merge of the old-command rows. Drop the merge; the
            // follow-up slot redoes it.
            if(guards.superseded && guards.superseded()) {
                LOG_INFO("Discarding superseded index result for {}", file_path);
                landed[path_id] = {.verdict = Verdict::Skipped};
                co_return RoundOutcome::Stale;
            }
            ScopedTimer merge_timer;
            auto report = store.merge(index_bytes.data(), index_bytes.size(), imports);
            if(!report) {
                if(report.error() == IndexStore::MergeError::Outdated) {
                    send_in_full.insert(path_id);
                    landed[path_id] = {.verdict = Verdict::Preempted,
                                       .error = "a variant the result named by hash is not stored"};
                    co_return RoundOutcome::Stale;
                }
                // Rejected wholesale: the file's rows are missing or stale,
                // which is a failure, not a completed index.
                landed[path_id] = {.verdict = Verdict::Failed,
                                   .error = "the TUIndex result failed verification"};
                co_return RoundOutcome::Failed;
            }
            // Record the borrowed host only for rows that landed: written at
            // dispatch, a failed rebuild would leave the persisted CDB
            // snapshot naming the new host while the retained rows were
            // built through the old one — an unchanged new host then pins
            // those stale rows fresh across restarts.
            if(resolved.source == CommandSource::IncludeGraph) {
                store.record_header_host(path_id, resolved.host);
            } else {
                store.forget_header_host(path_id);
            }
            outcome.report = std::move(*report);
            outcome.perf.merge_ms = merge_timer.ms();
        }
        landed[path_id] = std::move(outcome);
        co_return RoundOutcome::Success;
    }

    if(result.has_value()) {
        landed[path_id] = {.verdict = Verdict::Failed, .error = result.value().error};
        co_return RoundOutcome::Failed;
    }
    if(result.error().code == worker::dispatch_errc::cancelled) {
        landed[path_id] = {.verdict = Verdict::Preempted, .error = result.error().message};
        co_return RoundOutcome::Stale;
    }
    if(result.error().code == worker::dispatch_errc::worker_crashed) {
        landed[path_id] = {.verdict = Verdict::Crashed,
                           .error = "it crashed the worker: " + result.error().message};
        co_return RoundOutcome::Stale;
    }
    if(result.error().code == worker::dispatch_errc::worker_died ||
       result.error().code == worker::dispatch_errc::worker_lost) {
        landed[path_id] = {.verdict = Verdict::Lost, .error = result.error().message};
        co_return RoundOutcome::Stale;
    }
    if(result.error().code == worker::dispatch_errc::worker_unavailable && pool.revives_slots()) {
        // The outage is a window, not a verdict: the pool revives dead
        // slots, so the requeued attempt can succeed once one returns to
        // service. Without revival the failure below is terminal.
        landed[path_id] = {.verdict = Verdict::Preempted, .error = result.error().message};
        co_return RoundOutcome::Stale;
    }
    landed[path_id] = {.verdict = Verdict::Failed, .error = result.error().message};
    co_return RoundOutcome::Failed;
}

}  // namespace clice
