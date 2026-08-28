#include "server/service/ast_family.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "command/argument_parser.h"
#include "index/tu_index.h"
#include "sched/context.h"
#include "sched/families/build_common.h"
#include "server/protocol/position.h"
#include "server/service/context_service.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"
#include "worker/protocol.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "clang/Basic/Version.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

/// A quarantined document must not hide behind empty diagnostics: publish
/// one that says why every semantic feature went quiet, and how to lift it.
static kota::codec::RawValue quarantine_diagnostics(unsigned crashes) {
    std::vector<protocol::Diagnostic> diagnostics(1);
    auto& diagnostic = diagnostics[0];
    diagnostic.range = protocol::Range{
        .start = protocol::Position{.line = 0, .character = 0},
        .end = protocol::Position{.line = 0, .character = 0},
    };
    diagnostic.severity = protocol::DiagnosticSeverity::Error;
    diagnostic.source = "clice";
    diagnostic.message = std::format(
        "compiling this file crashed the language server worker {} times; "
        "the file is quarantined until it is edited",
        crashes);
    auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(diagnostics);
    return kota::codec::RawValue{json ? std::move(*json) : "[]"};
}

PCHPlan plan_pch(Workspace& workspace,
                 ContextResolver& contexts,
                 std::uint32_t path_id,
                 llvm::StringRef text,
                 const std::string& directory,
                 const std::vector<std::string>& arguments) {
    auto path = workspace.path_pool.resolve(path_id);
    auto bound = compute_preamble_bound(text);
    auto* header_context = contexts.header_context(path_id);
    bool has_prefix = header_context && !header_context->preamble_path.empty();
    if(bound == 0 && !has_prefix) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts land here too: they borrow
        // a command but inject nothing.
        return {};
    }

    // With a synthesized prefix, the PCH is worth building even at
    // bound == 0: the -include'd preamble file is processed via the
    // predefines buffer and lands in the PCH, so the (potentially huge)
    // prefix is not re-parsed on every edit. The -include flag is part of
    // the canonicalized arguments below, and the preamble file name is its
    // content hash, so the key tracks prefix changes automatically.

    // Key the PCH by preamble text plus the frontend-relevant compile flags,
    // so files with the same preamble text but different flags (-D, -I, -std)
    // produce separate PCHs.  The source file path stays out of the key so
    // files with identical preambles share one PCH — but its DIRECTORY (and
    // the working directory) must stay in: quote includes and relative paths
    // resolve against them, so equal preamble text in different directories
    // can mean different content.  The clang version guards against reusing
    // blobs a newer bundled clang would reject.
    auto preamble_text = text.substr(0, bound);
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              directory,
                              path::parent_path(path),
                              preamble_text,
                              canonicalize(arguments, ArgsProfile::Frontend)});
    return {
        .wanted = true,
        .request = {
                    .pch_key = std::move(pch_key),
                    .file = std::string(path),
                    .directory = directory,
                    .arguments = arguments,
                    .content = std::string(text),
                    .preamble_bound = bound,
                    }
    };
}

ASTFamily::ASTFamily(Workspace& workspace,
                     ContextResolver& contexts,
                     TaskGraph& graph,
                     PCMFamily& pcm,
                     PCHFamily& pch,
                     WorkerPool& pool,
                     SessionStore& sessions,
                     kota::event_loop& loop) :
    workspace(workspace), contexts(contexts), graph(graph), pcm(pcm), pch(pch), pool(pool),
    sessions(sessions), kicks(loop) {}

void ASTFamily::register_runner() {
    graph.register_family(ast_family, [this](RoundContext& ctx, NodeId id) {
        return run(ctx, static_cast<std::uint32_t>(id.key));
    });
}

void ASTFamily::publish_quarantined(const std::shared_ptr<Session>& session,
                                    std::optional<CommandSource> source,
                                    std::optional<std::uint32_t> line_limit) {
    session->quarantine.mark_announced();
    auto previous = projections.projection(session->path_id);
    bool has_previous = previous && previous->output.has_value();
    projections.set_output(session->path_id,
                           CompileOutput{
                               .version = std::nullopt,
                               .source = source.value_or(has_previous ? previous->output->source
                                                                      : CommandSource::CDBExact),
                               .diagnostics = quarantine_diagnostics(session->quarantine.crashes()),
                               .line_limit = line_limit,
                           });
    on_output.emit(session);
}

void ASTFamily::publish_recovered(const std::shared_ptr<Session>& session) {
    auto previous = projections.projection(session->path_id);
    bool has_previous = previous && previous->output.has_value();
    projections.set_output(
        session->path_id,
        CompileOutput{
            .version = std::nullopt,
            .source = has_previous ? previous->output->source : CommandSource::CDBExact,
            .diagnostics = kota::codec::RawValue{},
            .line_limit = std::nullopt,
        });
    on_output.emit(session);
}

void ASTFamily::publish_output(const std::shared_ptr<Session>& session, CompileOutput output) {
    projections.set_output(session->path_id, std::move(output));
    on_output.emit(session);
}

bool ASTFamily::is_stale(const Session& session) {
    auto it = projections.entries.find(session.path_id);
    if(it != projections.entries.end() && it->second.deps.has_value() &&
       deps_changed(workspace.path_pool, *it->second.deps)) {
        return true;
    }

    // Chain files of a header context are embedded in the synthesized
    // preamble, invisible to the deps snapshot — check them explicitly.
    if(auto* header_context = contexts.header_context(session.path_id);
       header_context && deps_changed(workspace.path_pool, header_context->deps)) {
        return true;
    }

    // Check PCH staleness via the projection's pch_key.
    auto projection = projections.projection(session.path_id);
    if(projection && projection->pch_key.has_value()) {
        auto pch_it = workspace.pch_cache.find(*projection->pch_key);
        if(pch_it != workspace.pch_cache.end() &&
           deps_changed(workspace.path_pool, pch_it->second.deps)) {
            return true;
        }
    }

    return false;
}

void ASTFamily::touch(std::uint32_t path_id) {
    auto& entry = projections.entries[path_id];
    entry.current = false;
    entry.epoch += 1;
}

void ASTFamily::supersede(std::uint32_t path_id) {
    touch(path_id);
    graph.update(node(path_id));
    // Not a wire cancel: the notification flips the compile's stop flag
    // and the round still observes its real reply (crash accounting
    // depends on it — contract 2). FIFO order puts it ahead of any
    // replacement Compile, which can only enter the pipe after this
    // round lands.
    if(graph.is_compiling(node(path_id))) {
        pool.notify_stateful(
            path_id,
            worker::CancelCompileParams{std::string(workspace.path_pool.resolve(path_id))});
    }
}

void ASTFamily::invalidate(std::uint32_t path_id) {
    touch(path_id);
    // The in-flight round's token fires, but its parse keeps running: the
    // buffer is unchanged, so the product is worth publishing as bounded
    // staleness before the round reports Stale.
    graph.update(node(path_id));
}

void ASTFamily::drop(std::uint32_t path_id) {
    graph.update(node(path_id));
    projections.entries.erase(path_id);
}

void ASTFamily::switch_identity(Session& session) {
    // Invalidate any in-flight compile: without the bump it would pass
    // its landing validity check and publish results for the superseded
    // identity.
    session.generation += 1;
    session.trial_done = false;
    auto& entry = projections.entries[session.path_id];
    if(entry.projection) {
        auto next = ASTProjection(*entry.projection);
        next.pch_key.reset();
        entry.projection = std::make_shared<const ASTProjection>(std::move(next));
    }
    entry.deps.reset();
    supersede(session.path_id);
}

void ASTFamily::escalate(Session& session) {
    if(session.serving != ServingMode::IndexOnly) {
        return;
    }
    if(readonly == ReadonlyMode::On) {
        return;
    }
    session.serving = ServingMode::Escalated;
}

void ASTFamily::request_compile(std::shared_ptr<Session> session) {
    auto kick = [](ASTFamily& self, std::shared_ptr<Session> session) -> kota::task<> {
        co_await self.ensure_compiled(std::move(session));
    };
    if(!kicks.spawn(kick(*this, std::move(session)))) {
        LOG_WARN("request_compile: task group stopped, dropping kick");
    }
}

kota::task<> ASTFamily::stop() {
    // Sessions, not projection entries: a first compile has no entry
    // until it lands, and its round is exactly the parse worth
    // interrupting.
    sessions.for_each([&](std::uint32_t path_id, const Session&) {
        if(graph.is_compiling(node(path_id))) {
            pool.notify_stateful(
                path_id,
                worker::CancelCompileParams{std::string(workspace.path_pool.resolve(path_id))});
        }
        return true;
    });
    kicks.cancel();
    co_await kicks.join();
}

kota::task<bool> ASTFamily::ensure_compiled(std::shared_ptr<Session> session) {
    auto path_id = session->path_id;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} current={}",
              path_id,
              session->version,
              session->generation,
              projections.current(path_id));

    // The crash budget lives on pool slots, the poison lives in documents:
    // a document that keeps killing workers is quarantined instead of
    // burning slot after slot. A content change grants one probe attempt.
    if(session->quarantine.blocked()) {
        LOG_WARN("ensure_compiled: {} quarantined after {} worker crashes",
                 workspace.path_pool.resolve(path_id),
                 session->quarantine.crashes());
        // A quarantine reached outside the compile-failure landing (a
        // completion or PCH build tipped the streak, or the crash landed on
        // a superseded generation) never materialized its diagnostic;
        // announce it once here instead of leaving the file silently dead.
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return false;
    }

    // The projection flag alone cannot clear this fast path: another
    // consumer's PCH staleness discovery dirties this node through the
    // graph cascade without touching the projection table, and a PCH
    // rebuilt before the next request would read fresh again here.
    if(projections.current(path_id) && !graph.is_dirty(node(path_id))) {
        if(!is_stale(*session)) {
            co_return true;
        }
        // A dependency changed on disk behind this session's back — the
        // lazy twin of the file tracker's DiskChanged. Route it through
        // the event pipeline (synchronous) so both share one cascade; for
        // an open file that dispatch invalidates the projection and
        // resets the trial. The dispatch re-resolves the session by
        // path_id; no suspension separates it from this frame, so it
        // finds the same open session this coroutine holds.
        on_stale(path_id);
    }

    // Join the document's round, retrying through stale attempts (a Lost
    // invalidation mid-flight lands Stale and the next attempt recompiles)
    // until a terminal outcome. A buffer edit or session replacement ends
    // the join instead: the result would describe a buffer that no longer
    // exists, and the editor re-requests after an edit.
    auto gen = session->generation;
    auto outcome = co_await graph.request(
        node(path_id),
        {.foreground = true, .validity = [session, gen] { return session->generation == gen; }});
    co_return outcome == JoinOutcome::Success;
}

kota::task<DependResult> ASTFamily::depend_modules(RoundContext& ctx,
                                                   std::uint32_t path_id,
                                                   llvm::StringRef directory,
                                                   const std::vector<std::string>& arguments,
                                                   llvm::StringRef text) {
    // A project with no module code pays nothing — no CDB lookup, no
    // precise scan. The moment import syntax exists anywhere (the
    // lexical candidate set), every document scans precisely: that is
    // the same cost the project pays once providers exist, and per-file
    // reachability approximations of "could this TU see an import" have
    // irreducible blind spots (macro includes, unsaved include chains).
    // The remaining gates catch what the candidate set cannot: this
    // buffer's own unsaved import, a header context's suffix, a forced
    // include. The scan's sentinel edges are what let the name's first
    // provider re-dirty this document.
    bool scan_worth = !workspace.path_to_module.empty() ||
                      !workspace.dep_graph.import_candidates().empty() ||
                      contexts.header_context(path_id) != nullptr ||
                      llvm::any_of(arguments, [](const std::string& arg) {
                          return llvm::StringRef(arg).starts_with("-include");
                      });
    if(!scan_worth) {
        scan_worth = scan_quick(text).has_import;
    }
    if(!scan_worth) {
        // The empty truth is still published: a durable import edge
        // earned earlier must stop cascading here, even when the compile
        // itself later fails (failed rounds keep declared edges).
        graph.declare(node(path_id), {});
        co_return DependResult::Ready;
    }

    // Imports come from the round's buffer snapshot under the round's own
    // resolved command — the same text and flags the parse will consume,
    // so a context choice or donated header host cannot enable an import
    // the scan missed. An unsaved `import m;` builds its PCM now, and the
    // next edit's round re-resolves — the superseded round's build loses
    // interest and winds down on its own (the PCH pattern). Module names
    // still resolve against the on-disk module map; a module unit is
    // somebody else's saved file. The remap stays engaged even for an
    // empty buffer: its own text has no imports, but the command's
    // injected -include prefix can still carry them.
    //
    // The import list is scanner truth, not build output: commit it as
    // durable edges before waiting, so a document whose compile fails
    // stays cascade-reachable from its imports — fixing or providing an
    // import must re-dirty the documents it broke. The per-round resolve
    // keeps the edges honest across CDB, buffer and import changes.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto deps = pcm.direct_deps(path_id, argv, directory, std::optional<llvm::StringRef>(text));
    graph.declare(node(path_id), deps.declared);
    // Sentinels join the round's candidates too: a successful landing
    // replaces the declaration with them, and a declare-only edge would
    // vanish with it.
    for(auto dep: deps.declared) {
        if(PCMFamily::is_unresolved(dep)) {
            ctx.reference(dep);
        }
    }
    if(deps.resolved.empty()) {
        co_return DependResult::Ready;
    }

    // Building a dependency can itself evict another clean module's PCM
    // under budget pressure, reopening the window the previous attempt's
    // revalidation closed — hence the bounded retry until the set is
    // stable. Past the bound the parse fails visibly on the missing file.
    for(int attempt = 0; attempt < 3; attempt += 1) {
        bool any_evicted = pcm.revalidate_blobs();
        if(attempt > 0 && !any_evicted) {
            break;
        }

        for(auto dep: deps.resolved) {
            switch(co_await ctx.depend({pcm_family, dep})) {
                case DependResult::Ready: break;
                case DependResult::Failed: co_return DependResult::Failed;
                case DependResult::Cancelled: co_return DependResult::Cancelled;
            }
        }
    }
    co_return DependResult::Ready;
}

kota::task<RoundOutcome> ASTFamily::run(RoundContext& ctx, std::uint32_t path_id) {
    // The session is resolved at round start: a didClose between spawn
    // and entry leaves nothing to compile.
    auto session = sessions.find(path_id);
    if(!session) {
        co_return RoundOutcome::Stale;
    }

    // The round's content identity. The graph's round identity covers
    // invalidation (ctx.current()); this covers the buffer itself — a
    // didChange or session replacement makes every product describe text
    // that no longer exists, so the landing discards wholesale, while a
    // Lost invalidation (generation unchanged) still publishes salvage.
    auto gen = session->generation;

    // Covers every spawn path, including waiter-driven respawns: content
    // that just spent its budget must not reach one more worker.
    if(session->quarantine.blocked()) {
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return RoundOutcome::Failed;
    }

    ScopedTimer timer;
    auto file_path = std::string(workspace.path_pool.resolve(path_id));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    LOG_INFO("compile round: starting path_id={} gen={}", path_id, gen);

    // The evidence this round inherited: a successful landing clears no
    // more (see Quarantine::land), so crashes recorded past this point — a
    // PCH build below, a concurrent completion build — keep accumulating
    // toward quarantine even when the compile itself lands.
    auto flight = session->quarantine.begin_flight();

    // At most two worker sends: a header with unknown self-containment
    // compiles without a prefix first; if the diagnostics indicate missing
    // includer context, the second send re-compiles with a synthesized
    // prefix. The trial's diagnostics are never published.
    bool artifact_retried = false;
    for(int attempt = 0; attempt < 2; attempt += 1) {
        worker::CompileParams params;
        params.path = file_path;
        params.version = session->version;
        params.text = session->text;
        auto source = contexts.resolve_command(file_path,
                                               params.directory,
                                               params.arguments,
                                               ContextUse::Editor);

        // The line the appended suffix #include lands on — anything at or
        // past it is phantom text the user cannot see.
        std::optional<std::uint32_t> suffix_line_limit;
        auto* header_context = contexts.header_context(path_id);
        if(header_context && !header_context->suffix_path.empty()) {
            auto newlines = std::ranges::count(params.text, '\n');
            suffix_line_limit =
                static_cast<std::uint32_t>(newlines + (params.text.ends_with('\n') ? 0 : 1));
        }
        contexts.append_suffix_include(path_id, params.text);

        // Whether this round is the self-containment probe: a header
        // deliberately compiled without its includer prefix to see if it
        // stands alone. Decided here, where resolve_command chose to omit
        // the prefix; the landing gates what the probe may write.
        bool trial_round = attempt == 0 && !session->trial_done && header_context &&
                           header_context->preamble_path.empty() &&
                           contexts.header_mode(file_path, path_id) == HeaderMode::Unknown;

        switch(co_await depend_modules(ctx,
                                       path_id,
                                       params.directory,
                                       params.arguments,
                                       params.text)) {
            case DependResult::Ready: break;
            case DependResult::Failed:
                LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
                co_return RoundOutcome::Failed;
            case DependResult::Cancelled: co_return RoundOutcome::Stale;
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before PCH for {}", uri_str);
            co_return RoundOutcome::Stale;
        }

        // Build or reuse the PCH through its family — the depend records
        // the Ast→Pch edge, so a staleness discovery on the shared pair
        // cascades here. Under readonly = "on" the build compiles without
        // a preamble instead — completion and signature help pay full
        // parses, the profile's stated trade. A failed pair is the same
        // degradation: the compile proceeds preamble-less.
        std::optional<std::string> adopted_pch;
        if(readonly != ReadonlyMode::On) {
            auto plan = plan_pch(workspace,
                                 contexts,
                                 path_id,
                                 params.text,
                                 params.directory,
                                 params.arguments);
            if(plan.wanted) {
                auto pch_key = plan.request.pch_key;
                if(!pch.fresh(pch_key) &&
                   !is_preamble_complete(params.text, plan.request.preamble_bound)) {
                    // Preamble incomplete (user still typing) and nothing
                    // fresh to adopt under the new key — defer the
                    // rebuild, keep using the previous PCH if it is still
                    // available.
                    LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", file_path);
                    if(auto previous = projections.projection(path_id);
                       previous && previous->pch_key.has_value()) {
                        adopted_pch = previous->pch_key;
                    }
                } else {
                    // This round is the dispatch owner when its depend
                    // spawns the PCH round: the probe pins every worker
                    // death of the build on this document (the preamble is
                    // its content), held by that round so the evidence
                    // lands even if this round goes stale meanwhile.
                    auto dep =
                        pch.prepare(std::move(plan.request), [session](llvm::StringRef death) {
                            session->quarantine.on_kind_crash(pch_evidence, death);
                        });
                    switch(co_await ctx.depend(dep)) {
                        case DependResult::Ready:
                            // Adoption is gated on the round outcome and on
                            // this round's own validity: a supersede or a
                            // Lost-type invalidation while we waited means
                            // the resolved command may describe nothing —
                            // neither the key nor the evidence wash belongs
                            // to this round anymore.
                            if(session->generation == gen && ctx.current()) {
                                adopted_pch = pch_key;
                                // Adopting a proven-good artifact disproves
                                // the session's PCH strikes as surely as
                                // building one — but only its own; every
                                // consumer washes for itself.
                                session->quarantine.on_kind_land(pch_evidence);
                            }
                            break;
                        case DependResult::Failed: break;
                        case DependResult::Cancelled: co_return RoundOutcome::Stale;
                    }
                }
            }
        }
        if(adopted_pch.has_value()) {
            if(auto pch_it = workspace.pch_cache.find(*adopted_pch);
               pch_it != workspace.pch_cache.end() && !pch_it->second.path.empty()) {
                params.pch = {pch_it->second.path, pch_it->second.bound};
            } else {
                adopted_pch.reset();
            }
        }

        // Fill all available PCM paths, excluding the file's own PCM
        // to avoid "multiple module declarations".
        workspace.fill_pcm_deps(params.pcms, path_id);

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before send for {}", uri_str);
            co_return RoundOutcome::Stale;
        }

        // A PCH crash above may have tipped the document into quarantine —
        // the entry gate ran before the streak grew. Stop before the
        // stateful dispatch instead of feeding the same content to one
        // more worker; the crash also spends any armed probe, since this
        // WAS the probe's attempt. A probe whose PCH build survived
        // (streak unchanged) continues to the compile.
        if(session->quarantine.active() && session->quarantine.grew(flight)) {
            LOG_WARN("compile round: {} quarantined during dependency prep", uri_str);
            session->quarantine.spend_probe();
            publish_quarantined(session, source, suffix_line_limit);
            co_return RoundOutcome::Failed;
        }

        // Seed the worker's inactive-region state from the PCH's preamble:
        // the conditional stack it left open (a #if cut by the bound) and
        // the regions of the preamble share itself. Copy the state out:
        // concurrent compiles can insert into pch_cache across the await
        // below and rehash the map from under a held pointer.
        std::shared_ptr<index::TUIndex> preamble_state;
        if(adopted_pch.has_value()) {
            preamble_state = workspace.preamble_state(*adopted_pch);
        }
        if(preamble_state) {
            auto regions = preamble_state->inactive_regions();
            params.preamble_inactive_regions.assign(regions.begin(), regions.end());
            auto conditionals = preamble_state->open_conditionals();
            params.open_conditionals.assign(conditionals.begin(), conditionals.end());
        }

        // The probe rides the dispatch that can disprove its evidence: a
        // compile spends it only when compiles are the crashers. A
        // kind-quarantined document's compile is ordinary work, and the
        // probe must survive it for the crashing kind's own retry.
        bool recovery = session->quarantine.recovery_compile();
        auto suspect = recovery ? Suspect::Isolated : Suspect::No;
        if(recovery) {
            session->quarantine.spend_probe();
        }
        // The send deliberately carries no token: the master must observe
        // the request's real outcome — the crash accounting below depends
        // on it (contract 2). A supersede interrupts the worker with a
        // CancelCompile notification instead (see supersede/stop), and
        // the stale reply is discarded at the validity gate below.
        auto result = co_await pool.send_stateful(path_id, params, {}, suspect);

        // Crash accounting runs even for superseded rounds: the crash came
        // from content this document dispatched, and skipping it would let a
        // poison file dodge quarantine by being edited between dispatch and
        // the crash response. Only a real mid-request death counts — a
        // restarting-window fast-fail never reached a worker.
        if(!result.has_value()) {
            if(result.error().code == worker::dispatch_errc::worker_crashed) {
                session->quarantine.on_crash(worker::death_of(result.error()));
            } else if(suspect == Suspect::Isolated &&
                      result.error().code == worker::dispatch_errc::worker_unavailable) {
                // The probe never ran: keep it armed so a later request
                // retries once an expendable worker frees up.
                session->quarantine.re_arm_probe();
            }
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded reply for {}", uri_str);
            co_return RoundOutcome::Stale;
        }

        if(!result.has_value()) {
            if(worker::is_operational_error(result.error())) {
                LOG_WARN("Compile did not complete for {}: {}", uri_str, result.error().message);
            } else {
                // The worker accepts arbitrary user code; a non-operational
                // failure at this layer is IPC/worker breakage, never a
                // user-code problem.
                LOG_ANOMALY(CompileFail,
                            "Compile failed for {}: {}",
                            uri_str,
                            result.error().message);
            }
            // A death while consuming a prebuilt pair may be the pair's
            // fault: deep corruption aborts the AST reader
            // (report_fatal_error in the bitstream reader) before any
            // diagnostic can anchor, so the attributable shapes above
            // never get a say. Retract the pair — a corrupt artifact then
            // heals on the next compile, while a genuinely poisonous
            // document keeps crashing and its own quarantine budget still
            // contains it (the extra rebuilds stay bounded by that
            // budget).
            if(result.error().code == worker::dispatch_errc::worker_crashed &&
               adopted_pch.has_value() && !params.pch.first.empty()) {
                LOG_WARN("Compile crashed consuming PCH pair {} for {}; retracting the pair",
                         *adopted_pch,
                         uri_str);
                pch.blame(*adopted_pch);
            }
            // A quarantined document announces itself instead of hiding
            // behind the empty list; the clear path publishes empty
            // diagnostics without a version.
            if(session->quarantine.active()) {
                publish_quarantined(session, source, suffix_line_limit);
            } else {
                publish_output(session,
                               CompileOutput{
                                   .version = std::nullopt,
                                   .source = source,
                                   .diagnostics = kota::codec::RawValue{},
                                   .line_limit = suffix_line_limit,
                               });
            }
            co_return RoundOutcome::Failed;
        }

        // The artifact quality gate: a failed parse whose diagnostics name
        // the consumed PCH (worker-side pch_suspect — setup failure and
        // fatal error alike). The family validated the pair fresh via its
        // deps, yet the frontend could not read it — the bytes on disk
        // are the suspect. Retract the pair (store + cache) and rerun the
        // round once; the next attempt misses and rebuilds both halves.
        // Without this write-back a corrupt blob is trusted for the life
        // of the store and the file stays broken on every restart. A
        // setup failure whose diagnostics do NOT name the blob (bad
        // invocation, broken module input) deliberately falls through to
        // the non-Done path below: retracting a healthy shared PCH over
        // someone else's failure would rebuild it on every request for as
        // long as that failure persists.
        if(result.value().pch_suspect && adopted_pch.has_value() && !params.pch.first.empty()) {
            LOG_WARN("Compile blamed PCH pair {} for {}; retracting the pair",
                     *adopted_pch,
                     uri_str);
            // Retract unconditionally — a blamed pair never survives, even
            // when the retry budget is spent — but rerun only once. The
            // strike ledger bounds the cross-round shape: this round lands
            // Stale (the retract re-dirties the key its candidate edge
            // points at), the waiter respawns it, and without the parked
            // key each respawn would rebuild and blame forever.
            pch.blame(*adopted_pch);
            if(!artifact_retried) {
                artifact_retried = true;
                // Rerun the same attempt so the trial semantics are
                // untouched (the loop counter is about probe rounds, not
                // artifact retries).
                attempt -= 1;
                continue;
            }
            // The rebuilt pair is blamed again: the storage itself is
            // failing, and another rebuild would fare no better. The
            // round proceeds — a Done reply publishes its real fatal
            // diagnostics, a non-Done one falls to the honest gap below.
        }

        // A non-Done reply past the gate is a non-result: settling it
        // would freeze the document on a product that never existed —
        // empty diagnostics, no index, an empty deps snapshot nothing can
        // invalidate. Superseded rounds were already discarded at the
        // validity gate above, so this round's inputs are broken in a
        // way a PCH rebuild cannot fix.
        if(result.value().status != worker::CompileStatus::Done) {
            LOG_WARN("Compile produced no result for {} (status={})",
                     uri_str,
                     static_cast<int>(result.value().status));
            // The projection stays non-current: the next request
            // recompiles instead of trusting the phantom product. Publish
            // the honest gap like the dispatch-failure path above —
            // versionless empty diagnostics rather than a stale list
            // posing as current.
            if(session->quarantine.active()) {
                publish_quarantined(session, source, suffix_line_limit);
            } else {
                publish_output(session,
                               CompileOutput{
                                   .version = std::nullopt,
                                   .source = source,
                                   .diagnostics = kota::codec::RawValue{},
                                   .line_limit = suffix_line_limit,
                               });
            }
            co_return RoundOutcome::Failed;
        }

        // A probe invalidated mid-flight is discarded whole: its verdict is
        // a conditional write like the projection's current flag (dispatch
        // reset trial_done and the header mode for the recompile to
        // re-earn), and its diagnostics come from a compile deliberately
        // run without includer context — they are never published,
        // including on this path.
        if(trial_round && !ctx.current()) {
            LOG_INFO("Discarding invalidated self-containment probe for {}", uri_str);
            co_return RoundOutcome::Stale;
        }

        // Self-containment trial verdict. Scored once per settled input
        // state: trial_done is reset whenever compile inputs change for
        // reasons other than buffer edits, so a dependency change re-runs
        // the trial while ordinary typing never does. Only NeedsContext is
        // persisted — SelfContained is recorded in memory alone (dependency
        // changes erase it) so queryContext can dedup identical-flag hosts
        // once the verdict is actually earned, never on a guess.
        if(trial_round) {
            std::vector<protocol::Diagnostic> diagnostics;
            if(!result.value().diagnostics.empty()) {
                [[maybe_unused]] auto status =
                    kota::codec::json::from_string(result.value().diagnostics.data, diagnostics);
            }
            session->trial_done = true;
            contexts.record_header_mode(path_id, HeaderMode::SelfContained);

            if(indicates_missing_context(diagnostics)) {
                LOG_INFO("Header {} needs includer context, re-compiling with prefix", uri_str);
                contexts.record_header_mode(path_id,
                                            HeaderMode::NeedsContext,
                                            hash_file(file_path));
                workspace.save_cache(contexts);
                contexts.drop_header_context(path_id);
                adopted_pch.reset();
                continue;
            }
        }

        // The landing: build the whole package off to the side, then
        // install it with no suspension before the outcome becomes
        // visible (contract 16) — a joiner must never see Success before
        // the projection describes the buffer it compiled. A Lost
        // invalidation mid-flight (ctx no longer current, generation
        // unchanged) lands the same package as bounded staleness —
        // recorded, published, but not current — and reports Stale so
        // waiters drive the recompile.
        bool current = ctx.current();

        // The parse consumed the pair and completed without blaming it —
        // and the round still describes live inputs: the key's strikes
        // were transient, not the storage. A voided round earns no
        // acquittal: the void may BE a fresh blame from another consumer
        // of the shared key, and clearing here would reset the strike
        // count it just paid for.
        if(current && adopted_pch.has_value() && !params.pch.first.empty() &&
           !result.value().pch_suspect) {
            pch.consumed_ok(*adopted_pch);
        }

        auto& index_data = result.value().tu_index_data;
        auto next = std::make_shared<ASTProjection>();
        next->pch_key = adopted_pch;
        // The AST and the file index settle together — that pairing is
        // what lets navigation trust the index after ensure_compiled. A
        // compile that produced no index data (fatal error, no AST) must
        // therefore drop the previous buffer's index rather than leave it
        // posing as current: an honest gap over yesterday's offsets.
        if(!index_data.empty()) {
            next->index = std::make_shared<index::TUIndex>(
                index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(index_data)));
        }

        LOG_PERF("request", "kind=Compile file={} total_ms={:.2f}", file_path, timer.ms_f());
        next->output = CompileOutput{
            .version = session->version,
            .source = source,
            .diagnostics = std::move(result.value().diagnostics),
            .line_limit = suffix_line_limit,
        };

        auto& entry = projections.entries[path_id];
        entry.projection = std::move(next);
        entry.deps = capture_deps_snapshot(workspace.path_pool,
                                           result.value().deps,
                                           result.value().build_at);
        entry.current = current;
        session->quarantine.land(flight);
        on_output.emit(session);
        // The push above told clients to re-pull what the fresh AST now
        // answers better; one refresh per landing.
        session->index_served = false;
        if(on_indexing_needed) {
            on_indexing_needed();
        }
        co_return current ? RoundOutcome::Success : RoundOutcome::Stale;
    }
    // Every arm of the two-send loop lands or returns; the trial's
    // continue only fires on the first send, the artifact retry only once.
    std::unreachable();
}

}  // namespace clice
