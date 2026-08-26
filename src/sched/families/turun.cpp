#include "sched/families/turun.h"

#include <cassert>
#include <utility>

#include "sched/families/pcm.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/protocol.h"

namespace clice {

TURunFamily::TURunFamily(TaskGraph& graph,
                         Workspace& workspace,
                         ContextResolver& contexts,
                         PCMFamily& pcm,
                         IndexStore& store,
                         WorkerPool& pool) :
    graph(graph), workspace(workspace), contexts(contexts), pcm(pcm), store(store), pool(pool) {}

void TURunFamily::register_runner() {
    graph.register_family(turun_family, [this](RoundContext& ctx, NodeId id) {
        return round(ctx, static_cast<std::uint32_t>(id.key));
    });
}

kota::task<TURunFamily::Outcome> TURunFamily::run(std::uint32_t path_id, Plan plan, Guards guards) {
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

kota::task<RoundOutcome> TURunFamily::round(RoundContext& ctx, std::uint32_t path_id) {
    auto it = inputs.find(path_id);
    assert(it != inputs.end() && "a TURun round spawns only under run()'s stash");
    // Copied: the map may rehash under a concurrent run() for another
    // file while this round is suspended.
    auto [plan, guards] = it->second;

    auto file_path = std::string(workspace.path_pool.resolve(path_id));

    // For module interface units, wait on the unit's own PCM node first:
    // its round builds the transitive imports and registers their
    // artifacts, so the stateless worker below has what it needs. An
    // ordinary TU in a module project waits on its imports the same way —
    // the fill_pcm_deps snapshot below would otherwise race a cold build
    // and parse without the module files. A failed PCM build is not
    // terminal here — the parse consumes whatever artifacts landed and
    // the worker reports its own failure if they are not enough.
    if(workspace.path_to_module.contains(path_id)) {
        if(co_await ctx.depend({pcm_family, path_id}) == DependResult::Cancelled) {
            landed[path_id] = {.verdict = Verdict::Preempted};
            co_return RoundOutcome::Stale;
        }
    } else if(!workspace.path_to_module.empty()) {
        for(auto dep: pcm.direct_deps(path_id)) {
            if(co_await ctx.depend({pcm_family, dep}) == DependResult::Cancelled) {
                landed[path_id] = {.verdict = Verdict::Preempted};
                co_return RoundOutcome::Stale;
            }
        }
    }

    worker::TURunParams params;
    params.file = file_path;
    params.index = plan.index;
    params.tidy = plan.tidy;
    params.tidy_checks = std::move(plan.tidy_params.checks);
    params.tidy_options = std::move(plan.tidy_params.options);
    params.tidy_warnings_as_errors = std::move(plan.tidy_params.warnings_as_errors);
    params.tidy_header_filter = std::move(plan.tidy_params.header_filter);
    params.tidy_system_headers = plan.tidy_params.system_headers;
    params.tidy_extra_args = std::move(plan.tidy_params.extra_args);
    params.tidy_extra_args_before = std::move(plan.tidy_params.extra_args_before);
    // Whole-TU runs stick to real commands; synthesized fallback commands
    // would fill the index (and the lint report) with guesses.
    std::uint32_t host_path_id = no_path_id;
    auto source = contexts.resolve_command(file_path,
                                           params.directory,
                                           params.arguments,
                                           ContextUse::Background,
                                           &host_path_id);
    if(source == CommandSource::Fallback) {
        // A file whose manifest survives keeps serving its last-known rows,
        // so skipping it loses nothing. One without a manifest (dropped or
        // never built) stays uncovered — count that as a failure so a batch
        // run reports the debt instead of exiting clean.
        if(!workspace.project_index.manifests.contains(path_id)) {
            landed[path_id] = {.verdict = Verdict::Failed,
                               .error = "no compile command found; the file stays uncovered"};
            co_return RoundOutcome::Failed;
        }
        landed[path_id] = {.verdict = Verdict::Skipped};
        co_return RoundOutcome::Stale;
    }

    workspace.fill_pcm_deps(params.pcms);

    ScopedTimer timer;
    auto result = co_await pool.send_stateless(params, worker::Priority::Low, {}, ctx.token());
    if(result.has_value() && result.value().success) {
        auto run_ms = timer.ms();
        auto& value = result.value();
        if(plan.index && value.tu_index_data.empty()) {
            landed[path_id] = {.verdict = Verdict::Failed,
                               .error = "the worker returned no TUIndex"};
            co_return RoundOutcome::Failed;
        }
        Outcome outcome;
        outcome.verdict = Verdict::Completed;
        outcome.tidy_diagnostics = std::move(value.tidy_diagnostics);
        outcome.perf = {.bytes = value.tu_index_data.size(), .index_ms = run_ms, .merge_ms = 0};
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
            // Landing-time admission: the serving side re-arbitrates before
            // the merge lands — a session opened or diverged mid-flight
            // vetoes the rows exactly as it would have at dispatch.
            auto landing = guards.landing ? guards.landing() : Admission::Admit;
            if(landing != Admission::Admit) {
                LOG_INFO("Serving side vetoed the index result for {}", file_path);
                landed[path_id] = {.verdict = Verdict::Skipped, .landing = landing};
                co_return RoundOutcome::Stale;
            }
            ScopedTimer merge_timer;
            auto report = store.merge(value.tu_index_data.data(), value.tu_index_data.size());
            if(!report) {
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
            if(source == CommandSource::IncludeGraph) {
                store.record_header_host(path_id, host_path_id);
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
        landed[path_id] = {.verdict = Verdict::Crashed, .error = result.error().message};
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
