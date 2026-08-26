#include "server/compiler/compiler.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "command/argument_parser.h"
#include "index/tu_index.h"
#include "sched/context.h"
#include "sched/families/build_common.h"
#include "server/protocol/extension.h"
#include "server/protocol/position.h"
#include "server/service/context_service.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"
#include "worker/protocol.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "clang/Basic/Version.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
using serde_raw = kota::codec::RawValue;

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

/// Publish the quarantine diagnostic as the session's current output. The
/// single materialization point for quarantine visibility: a quarantined
/// document must never sit behind stale or missing diagnostics.
void Compiler::publish_quarantined(const std::shared_ptr<Session>& session,
                                   std::optional<CommandSource> source,
                                   std::optional<std::uint32_t> line_limit) {
    session->quarantine.mark_announced();
    session->output = CompileOutput{
        .version = std::nullopt,
        .source = source.value_or(session->output.has_value() ? session->output->source
                                                              : CommandSource::CDBExact),
        .diagnostics = quarantine_diagnostics(session->quarantine.crashes()),
        .line_limit = line_limit,
        .inactive_regions = std::nullopt,
    };
    on_output.emit(session);
}

void Compiler::publish_recovered(const std::shared_ptr<Session>& session) {
    session->output = CompileOutput{
        .version = std::nullopt,
        .source = session->output.has_value() ? session->output->source : CommandSource::CDBExact,
        .diagnostics = kota::codec::RawValue{},
        .line_limit = std::nullopt,
        .inactive_regions = std::nullopt,
    };
    on_output.emit(session);
}

/// Send a stateless request, resending once if the worker died mid-request.
/// The pool does not retry on its own — it marks the dead slot and surfaces
/// worker_crashed, so the resend lands on a healthy worker. Build tasks are
/// idempotent; one retry suffices, since a request that kills two workers in
/// a row is a poison workload that a third attempt would not survive either.
///
/// `on_crash` fires once per attempt that killed a worker — evidence is
/// counted per death, not per request, so a poison build that burns two
/// workers spends two strikes. Callers must count ONLY through it: the
/// returned error is the retry's status, which may not be a crash.
template <typename Params, typename OnCrash>
static kota::ipc::RequestResult<Params>
    send_stateless_retrying(WorkerPool& pool,
                            Params params,
                            worker::Priority priority,
                            OnCrash on_crash,
                            kota::ipc::request_options opts = {}) {
    auto result = co_await pool.send_stateless(params, priority, opts);
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        on_crash(result.error());
        result = co_await pool.send_stateless(params, priority, opts);
        if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
            on_crash(result.error());
        }
    }
    co_return std::move(result);
}

/// Every stateless build carrying an open document's content goes through
/// here: each worker kill is blamed on the session's ledger for `kind`
/// before the caller sees the result, so no site can forget the
/// accounting. Clearing the kind on success stays with the caller — it
/// must be guarded by the launch generation, or a stale reply would
/// launder evidence the new content recorded meanwhile. Grep for
/// build_for to enumerate every such site.
template <typename Params>
static kota::ipc::RequestResult<Params> build_for(WorkerPool& pool,
                                                  Session& session,
                                                  std::uint8_t kind,
                                                  Params params,
                                                  worker::Priority priority,
                                                  kota::ipc::request_options opts) {
    return send_stateless_retrying(
        pool,
        std::move(params),
        priority,
        [&session, kind](const kota::ipc::protocol::Error& error) {
            session.quarantine.on_kind_crash(kind, worker::death_of(error));
        },
        opts);
}

/// Evidence-kind discriminators for Quarantine's per-kind ledgers. Queries
/// and stateless builds share one space, offset so they cannot collide;
/// document links have no QueryKind and get their own slot. The stateless
/// slots keep the values of the retired BuildKind enum (0x40 + kind) —
/// they are in-memory only, but drift within a session would misattribute
/// evidence.
constexpr std::uint8_t evidence_kind(worker::QueryKind kind) {
    return static_cast<std::uint8_t>(kind);
}

constexpr inline std::uint8_t document_link_evidence = 0x20;
constexpr inline std::uint8_t pch_evidence = 0x40;
constexpr inline std::uint8_t completion_evidence = 0x43;
constexpr inline std::uint8_t signature_help_evidence = 0x44;
constexpr inline std::uint8_t format_evidence = 0x45;

Compiler::Compiler(kota::event_loop& loop,
                   Workspace& workspace,
                   ContextResolver& contexts,
                   PCMFamily& pcm,
                   PCHFamily& pch,
                   WorkerPool& pool) :
    loop(loop), workspace(workspace), contexts(contexts), pcm(pcm), pch(pch), pool(pool) {}

kota::task<> Compiler::stop() {
    compile_tasks.cancel();
    co_await compile_tasks.join();
}

/// The pch_key write license: a round may (re)write the session's PCH
/// reference only while BOTH staleness tokens still hold their takeoff
/// values. A supersede bumps generation; a Lost-type invalidation (disk or
/// CDB change behind an in-flight round) bumps only dirty_epoch — either
/// way the round's resolved directory/arguments may describe a command
/// that no longer exists, and writing its PCH key back would hand later
/// incomplete-preamble edits a stale-flag PCH.
static bool may_write_pch_key(const Session& session,
                              std::uint64_t launch_generation,
                              std::uint64_t launch_epoch) {
    return session.generation == launch_generation && session.dirty_epoch == launch_epoch;
}

kota::task<bool> Compiler::ensure_pch(const std::shared_ptr<Session>& session,
                                      std::uint64_t launch_generation,
                                      std::uint64_t launch_epoch,
                                      const std::string& directory,
                                      const std::vector<std::string>& arguments) {
    // A round invalidated during the caller's earlier awaits (module
    // dependencies) must not touch pch_key at all: the reset branch below
    // writes it before the first suspension point.
    if(!may_write_pch_key(*session, launch_generation, launch_epoch)) {
        co_return false;
    }

    auto path_id = session->path_id;
    auto path = workspace.path_pool.resolve(path_id);
    auto& text = session->text;
    auto bound = compute_preamble_bound(text);
    auto* header_context = contexts.header_context(path_id);
    bool has_prefix = header_context && !header_context->preamble_path.empty();
    if(bound == 0 && !has_prefix) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts land here too: they borrow
        // a command but inject nothing.
        session->pch_key.reset();
        co_return true;
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
    auto preamble_text = llvm::StringRef(text).substr(0, bound);
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              directory,
                              path::parent_path(path),
                              preamble_text,
                              canonicalize(arguments, ArgsProfile::Frontend)});

    // Preamble incomplete (user still typing) and nothing fresh to adopt
    // under the new key — defer the rebuild, keep using the session's
    // previous PCH if it is still available.
    if(!pch.fresh(pch_key) && !is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        if(session->pch_key.has_value()) {
            auto it = workspace.pch_cache.find(*session->pch_key);
            co_return it != workspace.pch_cache.end() && !it->second.path.empty();
        }
        co_return false;
    }

    // Revalidate or build through the family. This frame is the dispatch
    // owner when its request spawns the round; the probe then pins every
    // worker death of the build on this document (the preamble is its
    // content), held by the round so the evidence lands even if this
    // frame's launch goes stale meanwhile — a stale round's crashes still
    // count. Joiners of an already-running round install nothing and
    // replay nothing.
    auto outcome = co_await pch.acquire({.pch_key = pch_key,
                                         .file = std::string(path),
                                         .directory = directory,
                                         .arguments = arguments,
                                         .content = text,
                                         .preamble_bound = bound},
                                        [session](llvm::StringRef death) {
                                            session->quarantine.on_kind_crash(pch_evidence, death);
                                        });
    if(outcome != PCHFamily::Outcome::Ready) {
        co_return false;
    }

    // Adoption is gated on the round outcome, never on leftover cache
    // paths, and on this launch's own validity: a supersede or a
    // Lost-type invalidation while we waited means the resolved command
    // may describe nothing — neither the pch_key write nor the evidence
    // wash belongs to this launch anymore.
    if(!may_write_pch_key(*session, launch_generation, launch_epoch)) {
        co_return false;
    }
    session->pch_key = pch_key;
    // Adopting a proven-good artifact disproves the session's PCH strikes
    // as surely as building one — but only its own; every joiner washes
    // for itself.
    session->quarantine.on_kind_land(pch_evidence);
    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
kota::task<bool> Compiler::ensure_deps(const std::shared_ptr<Session>& session,
                                       std::uint64_t launch_generation,
                                       std::uint64_t launch_epoch,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments,
                                       std::pair<std::string, uint32_t>& pch_pair,
                                       std::unordered_map<std::string, std::string>& pcms,
                                       std::optional<kota::cancellation_token> scope) {
    auto path_id = session->path_id;

    // Prepare module dependencies within the request scope: cancelling the
    // scope unwinds the wait and releases this request's interest in the
    // dependency graph, without touching the shared compilations themselves.
    // A user request waits on these builds: dispatch them High so the
    // background budget cannot throttle its own foreground.
    auto scoped_deps = [&](kota::task<bool> wait) -> kota::task<bool> {
        if(!scope) {
            co_return co_await std::move(wait);
        }
        auto result = co_await kota::with_token(std::move(wait), *scope);
        co_return result.has_value() && *result;
    };

    if(!co_await scoped_deps(pcm.prepare_deps(path_id, /*foreground=*/true))) {
        co_return false;
    }

    // Scan buffer text for module imports that might not be in compile_graph yet.
    // When a user adds `import std;` without saving, the compile_graph (disk-based)
    // doesn't know about the new dependency. Scan the in-memory text to find them.
    //
    // FIXME: dead code — scan_quick never reports imports (its contract:
    // import tokens are macro-expanded, so the lexer level cannot see
    // them), so this loop body never runs and unsaved `import` additions
    // are never discovered. Needs the precise scan over the buffer text
    // when module support resumes.
    {
        auto scan_result = scan_quick(session->text);
        for(auto& mod_name: scan_result.modules) {
            if(mod_name.empty())
                continue;
            // Finish the map lookup before suspending: compile_deps below
            // awaits, and a concurrent didSave can mutate path_to_module,
            // invalidating any iterator/reference held across the suspension.
            bool found = false;
            std::uint32_t module_pid = 0;
            for(auto& [pid, name]: workspace.path_to_module) {
                if(name == mod_name) {
                    module_pid = pid;
                    found = true;
                    break;
                }
            }
            if(!found) {
                LOG_DEBUG("Buffer imports unknown module '{}', skipping", mod_name);
                continue;
            }
            // If PCM not already built, try to build it.
            if(workspace.pcm_paths.find(module_pid) == workspace.pcm_paths.end()) {
                if(pcm.tracks(module_pid)) {
                    co_await scoped_deps(pcm.build_deps(module_pid, /*foreground=*/true));
                }
            }
        }
    }

    // The buffer-scan waits above tolerate failed PCM builds, but a cancelled
    // scope means this round was superseded — abandon it before the PCH step.
    if(scope && scope->cancelled()) {
        co_return false;
    }

    // Build or reuse PCH. Under readonly = "on" the build compiles
    // without a preamble instead — completion and signature help pay full
    // parses, the profile's stated trade.
    if(readonly != ReadonlyMode::On) {
        auto pch_ok =
            co_await ensure_pch(session, launch_generation, launch_epoch, directory, arguments);
        if(pch_ok && session->pch_key.has_value()) {
            if(auto pch_it = workspace.pch_cache.find(*session->pch_key);
               pch_it != workspace.pch_cache.end()) {
                pch_pair = {pch_it->second.path, pch_it->second.bound};
            }
        }
    }

    // Fill all available PCM paths, excluding the file's own PCM
    // to avoid "multiple module declarations".
    workspace.fill_pcm_deps(pcms, path_id);

    co_return true;
}

bool Compiler::is_stale(Session& session) {
    if(session.ast_deps.has_value() && deps_changed(workspace.path_pool, *session.ast_deps))
        return true;

    // Chain files of a header context are embedded in the synthesized
    // preamble, invisible to ast_deps — check them explicitly.
    if(auto* header_context = contexts.header_context(session.path_id);
       header_context && deps_changed(workspace.path_pool, header_context->deps))
        return true;

    // Check PCH staleness via the session's pch_key.
    if(session.pch_key.has_value()) {
        auto pch_it = workspace.pch_cache.find(*session.pch_key);
        if(pch_it != workspace.pch_cache.end() &&
           deps_changed(workspace.path_pool, pch_it->second.deps))
            return true;
    }

    return false;
}

void Compiler::record_deps(Session& session, llvm::ArrayRef<DepFile> deps, std::int64_t build_at) {
    session.ast_deps = capture_deps_snapshot(workspace.path_pool, deps, build_at);
}

/// Pull-based compilation entry point for user-opened files.
///
/// Called lazily by forward_query() / forward_interactive() before every
/// feature request (hover, semantic tokens, etc.). Guarantees that when it
/// returns true the stateful worker assigned to `path_id` holds an up-to-date
kota::task<> Compiler::run_compile(std::shared_ptr<Session> session) {
    auto pc = session->compiling;
    auto pid = session->path_id;
    auto gen = session->generation;
    // Takeoff snapshot for the conditional dirty-flag clear on landing
    // (see Session::settle_compile). The generation checks below answer
    // "is the buffer still the same buffer"; this answers "did the world
    // get dirty again while we were flying".
    auto epoch = session->dirty_epoch;

    // RAII, not a manual call: kotatsu cancellation destroys a suspended
    // frame without resuming it (compile_tasks.cancel() at shutdown), and
    // a finish that never runs would leave session->compiling set and
    // `done` unset — every waiter in ensure_compiled hangs and the session
    // is bricked. Same pattern as BuildingGuard and UnitGuard.
    struct [[nodiscard]] FinishCompile {
        std::shared_ptr<Session>& session;
        std::shared_ptr<Session::PendingCompile>& pc;
        std::uint32_t pid;

        ~FinishCompile() {
            if(session->compiling == pc) {
                session->compiling.reset();
            }
            LOG_INFO("ensure_compiled: finish path_id={}", pid);
            pc->done.set();
        }
    } finish_compile{session, pc, pid};

    LOG_INFO("ensure_compiled: starting compile path_id={} gen={}", pid, gen);

    ScopedTimer timer;
    auto file_path = std::string(workspace.path_pool.resolve(pid));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    // The evidence this request inherited: a successful landing clears no
    // more (see Quarantine::land), so crashes recorded past this point — a
    // PCH build inside ensure_deps, a concurrent completion build — keep
    // accumulating toward quarantine even when the compile itself lands.
    auto flight = session->quarantine.begin_flight();

    // At most two rounds: a header with unknown self-containment compiles
    // without a prefix first; if the diagnostics indicate missing includer
    // context, the second round re-compiles with a synthesized prefix.
    // The trial's diagnostics are never published.
    bool artifact_retried = false;
    for(int attempt = 0; attempt < 2; ++attempt) {
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
        auto* header_context = contexts.header_context(session->path_id);
        if(header_context && !header_context->suffix_path.empty()) {
            auto newlines = std::ranges::count(params.text, '\n');
            suffix_line_limit =
                static_cast<std::uint32_t>(newlines + (params.text.ends_with('\n') ? 0 : 1));
        }
        contexts.append_suffix_include(session->path_id, params.text);

        // Whether this round is the self-containment probe: a header
        // deliberately compiled without its includer prefix to see if it
        // stands alone. Decided here, where resolve_command chose to omit
        // the prefix; the landing gates what the probe may write.
        bool trial_round = attempt == 0 && !session->trial_done && header_context &&
                           header_context->preamble_path.empty() &&
                           contexts.header_mode(file_path, pid) == HeaderMode::Unknown;

        bool deps_ok = co_await ensure_deps(session,
                                            gen,
                                            epoch,
                                            params.directory,
                                            params.arguments,
                                            params.pch,
                                            params.pcms,
                                            pc->deps_scope.token());
        if(!deps_ok) {
            LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
            co_return;
        }

        if(session->generation != gen) {
            LOG_INFO("ensure_compiled: superseded before send ({} vs {}) for {}",
                     session->generation,
                     gen,
                     uri_str);
            co_return;
        }

        // The pair this round consumes, snapshotted at dispatch: the
        // reply handling below must retract what it actually used —
        // pch_key can be rewritten while the send is suspended (a context
        // switch, a concurrent completion round), and blaming the current
        // key would delete an unrelated pair.
        auto dispatched_pch_key = session->pch_key;

        // A PCH crash inside ensure_deps may have tipped the document into
        // quarantine — the entry gate ran before the streak grew. Stop
        // before the stateful dispatch instead of feeding the same content
        // to one more worker; the crash also spends any armed probe, since
        // this WAS the probe's attempt. A probe whose PCH build survived
        // (streak unchanged) continues to the compile.
        if(session->quarantine.active() && session->quarantine.grew(flight)) {
            LOG_WARN("ensure_compiled: {} quarantined during dependency prep", uri_str);
            session->quarantine.spend_probe();
            publish_quarantined(session, source, suffix_line_limit);
            co_return;
        }

        // Seed the inactive-region scan with the conditional stack the
        // PCH's preamble left open (a #if cut by the bound). Copy the state
        // out: concurrent compiles can insert into pch_cache across the
        // await below and rehash the map from under a held pointer.
        std::vector<std::uint32_t> pch_inactive;
        std::shared_ptr<index::TUIndex> preamble_state;
        if(session->pch_key.has_value()) {
            preamble_state = workspace.preamble_state(*session->pch_key);
        }
        if(preamble_state) {
            auto regions = preamble_state->inactive_regions();
            pch_inactive.assign(regions.begin(), regions.end());
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
        // The send deliberately does NOT run under the supersede scope: the
        // master must observe the request's real outcome — the crash
        // accounting below depends on it (a wire cancel racing a worker
        // death would resume with RequestCancelled and the death would
        // dodge the document's ledger). A supersede interrupts the worker
        // with a CancelCompile notification instead, and the stale reply is
        // discarded at the generation gate below.
        auto result = co_await pool.send_stateful(pid, params, {}, suspect);

        // Crash accounting runs even for superseded compiles: the crash came
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
            LOG_INFO("ensure_compiled: generation mismatch ({} vs {}) for {}",
                     session->generation,
                     gen,
                     uri_str);
            co_return;
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
               dispatched_pch_key.has_value() && !params.pch.first.empty()) {
                LOG_WARN("Compile crashed consuming PCH pair {} for {}; retracting the pair",
                         *dispatched_pch_key,
                         uri_str);
                pch.invalidate(*dispatched_pch_key);
                if(session->pch_key == dispatched_pch_key) {
                    session->pch_key.reset();
                }
            }
            // A quarantined document announces itself instead of hiding
            // behind the empty list; the clear path publishes empty
            // diagnostics without a version and no inactive regions.
            if(session->quarantine.active()) {
                publish_quarantined(session, source, suffix_line_limit);
            } else {
                session->output = CompileOutput{
                    .version = std::nullopt,
                    .source = source,
                    .diagnostics = kota::codec::RawValue{},
                    .line_limit = suffix_line_limit,
                    .inactive_regions = std::nullopt,
                };
                on_output.emit(session);
            }
            co_return;
        }

        // The artifact quality gate: a failed parse whose diagnostics name
        // the consumed PCH (worker-side pch_suspect — setup failure and
        // fatal error alike). ensure_pch validated the pair fresh via its
        // deps, yet the frontend could not read it — the bytes on disk
        // are the suspect. Retract the pair (store + cache) and rerun the
        // round once; ensure_pch misses and rebuilds both halves. Without
        // this write-back a corrupt blob is trusted for the life of the
        // store and the file stays broken on every restart. A setup
        // failure whose diagnostics do NOT name the blob (bad invocation,
        // broken module input) deliberately falls through to the non-Done
        // path below: retracting a healthy shared PCH over someone else's
        // failure would rebuild it on every request for as long as that
        // failure persists.
        if(result.value().pch_suspect && dispatched_pch_key.has_value() &&
           !params.pch.first.empty()) {
            LOG_WARN("Compile blamed PCH pair {} for {}; retracting the pair",
                     *dispatched_pch_key,
                     uri_str);
            // Retract unconditionally — a blamed pair never survives, even
            // when the retry budget is spent — but rerun only once.
            pch.invalidate(*dispatched_pch_key);
            if(session->pch_key == dispatched_pch_key) {
                session->pch_key.reset();
            }
            if(!artifact_retried) {
                artifact_retried = true;
                // Rerun the same attempt so the trial semantics are
                // untouched (the loop counter is about probe rounds, not
                // artifact retries).
                --attempt;
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
        // generation gate above, so this round's inputs are broken in a
        // way a PCH rebuild cannot fix.
        if(result.value().status != worker::CompileStatus::Done) {
            LOG_WARN("Compile produced no result for {} (status={})",
                     uri_str,
                     static_cast<int>(result.value().status));
            // ast_dirty stays set: the next request recompiles instead of
            // trusting the phantom product. Publish the honest gap like
            // the dispatch-failure path above — versionless empty
            // diagnostics rather than a stale list posing as current.
            if(session->quarantine.active()) {
                publish_quarantined(session, source, suffix_line_limit);
            } else {
                session->output = CompileOutput{
                    .version = std::nullopt,
                    .source = source,
                    .diagnostics = kota::codec::RawValue{},
                    .line_limit = suffix_line_limit,
                    .inactive_regions = std::nullopt,
                };
                on_output.emit(session);
            }
            co_return;
        }

        // A probe invalidated mid-flight is discarded whole: its verdict is
        // a conditional write like the dirty flag (dispatch reset trial_done
        // and the header mode for the recompile to re-earn), and its
        // diagnostics come from a compile deliberately run without includer
        // context — they are never published, including on this path.
        // ast_dirty is still set, so the next request re-runs the trial.
        if(trial_round && session->dirty_epoch != epoch) {
            LOG_INFO("Discarding invalidated self-containment probe for {}", uri_str);
            co_return;
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
            contexts.record_header_mode(pid, HeaderMode::SelfContained);

            if(indicates_missing_context(diagnostics)) {
                LOG_INFO("Header {} needs includer context, re-compiling with prefix", uri_str);
                contexts.record_header_mode(pid, HeaderMode::NeedsContext, hash_file(file_path));
                workspace.save_cache(contexts);
                contexts.drop_header_context(pid);
                session->pch_key.reset();
                continue;
            }
        }

        // Conditional write: if an invalidation landed mid-flight (a header
        // was saved, the document was evicted, ...) this product describes
        // a stale world — record it, publish it (bounded staleness), but do
        // not declare it fresh; the next request recompiles.
        session->settle_compile(epoch);
        session->quarantine.land(flight);
        pc->succeeded = true;
        record_deps(*session, result.value().deps, result.value().build_at);

        // The AST and the file index settle together — that pairing is
        // what lets navigation trust the index after ensure_compiled. A
        // compile that produced no index data (fatal error, no AST) must
        // therefore drop the previous buffer's index rather than leave it
        // posing as current: an honest gap over yesterday's offsets.
        auto& index_data = result.value().tu_index_data;
        session->index =
            index_data.empty()
                ? index::TUIndex()
                : index::TUIndex::from_buffer(llvm::MemoryBuffer::getMemBufferCopy(index_data));

        auto version = session->version;

        LOG_PERF("request", "kind=Compile file={} total_ms={:.2f}", file_path, timer.ms_f());
        // The preamble's share lives with the PCH; the compile result
        // covers the content past the bound. Publish both.
        auto inactive = std::move(pch_inactive);
        inactive.insert(inactive.end(),
                        result.value().inactive_regions.begin(),
                        result.value().inactive_regions.end());
        session->output = CompileOutput{
            .version = version,
            .source = source,
            .diagnostics = std::move(result.value().diagnostics),
            .line_limit = suffix_line_limit,
            .inactive_regions = std::move(inactive),
        };
        on_output.emit(session);
        // The push above told clients to re-pull what the fresh AST now
        // answers better; one refresh per landing.
        session->index_served = false;
        if(on_indexing_needed)
            on_indexing_needed();
        co_return;
    }
}

void Compiler::escalate(Session& session) {
    if(session.serving != ServingMode::IndexOnly) {
        return;
    }
    if(readonly == ReadonlyMode::On) {
        return;
    }
    session.serving = ServingMode::Escalated;
}

void Compiler::request_compile(std::shared_ptr<Session> session) {
    auto kick = [](Compiler& self, std::shared_ptr<Session> session) -> kota::task<> {
        co_await self.ensure_compiled(std::move(session));
    };
    if(!compile_tasks.spawn(kick(*this, std::move(session)))) {
        LOG_WARN("request_compile: task group stopped, dropping kick");
    }
}

/// AST and diagnostics have been published to the client.
///
/// Lifecycle overview (pull-based model):
///
///   didOpen / didChange          – only update Session, mark ast_dirty
///   didSave                      – mark dependents dirty, queue indexing
///   feature request arrives      – calls ensure_compiled() first
///     1. Fast-path exit if AST is already clean (!ast_dirty).
///     2. Compile any C++20 module dependencies (PCMs) via CompileGraph.
///     3. Build / reuse the precompiled header (PCH) via ensure_pch().
///     4. Send CompileParams to the stateful worker, which builds the AST.
///     5. On success: publish diagnostics, clear ast_dirty, schedule indexing.
///     6. On generation mismatch (user edited during compile): keep dirty,
///        the next feature request will trigger another compile cycle.
///
/// Only the opened file itself is remapped (its in-memory text is sent to the
/// worker); every other file is read from disk by the compiler.
///
/// Concurrency: multiple concurrent feature requests for the same file will
/// each call ensure_compiled(). The first one spawns a compile task into the
/// Compiler's task_group; subsequent ones wait on the shared event.
/// The spawned task is not cancelled by LSP $/cancelRequest, preventing
/// the race where cancellation wakes all waiters and they all start compiles.
kota::task<bool> Compiler::ensure_compiled(std::shared_ptr<Session> session) {
    auto path_id = session->path_id;
    auto gen = session->generation;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} ast_dirty={}",
              path_id,
              session->version,
              gen,
              session->ast_dirty);

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

    if(!session->ast_dirty) {
        if(!is_stale(*session)) {
            co_return true;
        }
        // A dependency changed on disk behind this session's back — the
        // lazy twin of the file tracker's DiskChanged. Route it through
        // the event pipeline (synchronous) so both share one cascade; for
        // an open file that dispatch marks the AST dirty, resets the trial
        // and bumps dirty_epoch. The dispatch re-resolves the session by
        // path_id; no suspension separates it from this frame, so it finds
        // the same open session this coroutine holds.
        on_stale(path_id);
    }

    // If an up-to-date compile is already in flight, wait for it. The wait
    // may watch that compile spend the streak's last budget: re-check the
    // gate afterwards (below) before launching a replacement.
    // This co_await may be cancelled by LSP $/cancelRequest — that's fine,
    // it just means this particular feature request is abandoned.  The
    // detached compile task keeps running independently.
    while(session->compiling) {
        auto pending = session->compiling;
        if(pending->generation != session->generation) {
            // The in-flight compile is stale (user edited since it started):
            // supersede it. The launch below interrupts the worker's parse
            // with a CancelCompile notification and cancels deps_scope to
            // release the module-graph interest; the round itself still runs
            // to its (incomplete) reply so crash accounting sees the real
            // outcome.
            break;
        }
        co_await pending->done.wait();
        if(!session->ast_dirty)
            co_return true;
    }

    // If we fell through (not superseding) and the generation changed while
    // we were waiting, the session was closed or replaced — don't compile.
    if(!session->compiling && session->generation != gen) {
        co_return false;
    }

    // The compile just waited out may have spent the streak's last budget
    // (its crash, or its PCH build's): the entry gate ran before that
    // evidence existed, so a waiter must not launch a replacement for
    // content that is now quarantined. The crash's own error path already
    // announced it.
    if(session->quarantine.blocked()) {
        LOG_WARN("ensure_compiled: {} quarantined while waiting for a compile",
                 workspace.path_pool.resolve(path_id));
        co_return false;
    }

    auto superseded = session->compiling;

    // Interrupt the stale parse before the replacement can enter the pipe:
    // FIFO order guarantees the cancel reaches the worker ahead of the new
    // Compile request, so it can only ever hit the stale round's stop flag.
    interrupt_superseded(*session);

    auto pending_compile = std::make_shared<Session::PendingCompile>();
    pending_compile->generation = session->generation;
    session->compiling = pending_compile;

    LOG_INFO("ensure_compiled: launching compile path_id={} gen={}", path_id, session->generation);

    // Spawn the replacement before cancelling the superseded compile: the new
    // round acquires its module-dependency interest synchronously, so shared
    // dependencies never see their interest drop to zero across the swap.
    compile_tasks.spawn(run_compile(session));

    if(superseded) {
        superseded->deps_scope.cancel();
    }

    // Wait for the detached compile to finish.  If this wait is cancelled
    // by LSP $/cancelRequest, the detached task continues unaffected.
    co_await pending_compile->done.wait();

    co_return !session->ast_dirty;
}

void Compiler::interrupt_superseded(Session& session) {
    if(!session.compiling || session.compiling->generation == session.generation) {
        return;
    }
    // Not a wire cancel: the notification flips the compile's stop flag and
    // the request still completes into run_compile's crash accounting (see
    // the send site). A stale set is impossible — every emitter runs before
    // the replacement Compile can enter the pipe.
    pool.notify_stateful(
        session.path_id,
        worker::CancelCompileParams{std::string(workspace.path_pool.resolve(session.path_id))});
}

void Compiler::abandon_superseded(Session& session) {
    if(!session.compiling || session.compiling->generation == session.generation) {
        return;
    }
    interrupt_superseded(session);
    // The round may still be in dependency prep, where the notification
    // cannot reach it (nothing dispatched yet): cancel its waits so it
    // unwinds now instead of after the module graph settles. The cancel
    // cascade can finish the round synchronously — session.compiling may
    // be null when this returns. (A round inside its PCH build stays until
    // the shared build replies: that send is deliberately scope-free.)
    session.compiling->deps_scope.cancel();
}

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind,
                                            std::shared_ptr<Session> session,
                                            std::optional<protocol::Position> position,
                                            std::optional<protocol::Range> range,
                                            std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;
    auto map = session->line_map();

    ScopedTimer timer;
    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }
    auto wait_ms = timer.ms_f();

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;
    wp.config = workspace.config;

    if(position) {
        wp.offset = clamped_offset(map, *position);
    }

    if(range) {
        wp.range = {clamped_offset(map, range->start), clamped_offset(map, range->end)};
        if(wp.range.begin > wp.range.end) {
            co_return kota::outcome_error(
                kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                                 "Range start is after its end"});
        }
    }

    // This kind holding strikes without a probe is neither licensed
    // recovery nor safe ordinary work.
    if(session->quarantine.kind_blocked(evidence_kind(kind))) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    // A recovery query — this kind holds the strikes — is still
    // distrusted: it needs the owner (the AST lives there), but its crash
    // spends no slot budget and new documents avoid the worker while it
    // flies. The guard spends the probe the edit licensed (a harmless kind
    // must not: hover would strand a semantic-tokens quarantine) and hands
    // it back unless the attempt recorded a strike.
    bool recovery = session->quarantine.recovery_kind(evidence_kind(kind));
    auto suspect = recovery ? Suspect::InPlace : Suspect::No;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await pool.send_stateful(path_id, wp, {.token = std::move(token)}, suspect);
    if(!result.has_value()) {
        // A query that kills the worker is this document's doing even
        // though its compile landed: per-kind ledger, since only this query
        // kind answering disproves it (see Quarantine::on_kind_crash).
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            session->quarantine.on_kind_crash(evidence_kind(kind),
                                              worker::death_of(result.error()));
        }
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "query (kind={}) failed for {}: {}",
                        kind,
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The reply proves queries on the DISPATCHED content answer; an edit
    // that landed mid-flight must not launder the new content's ledger —
    // crashes count regardless of staleness, successes only when fresh.
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(evidence_kind(kind));
        if(was_active && !session->quarantine.active()) {
            publish_recovered(session);
        }
    }
    LOG_PERF("request",
             "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
             kind,
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
    Compiler::forward_document_links(std::shared_ptr<Session> session,
                                     std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    ScopedTimer timer;
    if(!co_await ensure_compiled(session)) {
        co_return std::vector<feature::DocumentLink>{};
    }
    if(session->generation != gen) {
        co_return std::vector<feature::DocumentLink>{};
    }
    auto wait_ms = timer.ms_f();

    if(session->quarantine.kind_blocked(document_link_evidence)) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    bool recovery = session->quarantine.recovery_kind(document_link_evidence);
    auto suspect = recovery ? Suspect::InPlace : Suspect::No;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await pool.send_stateful(path_id,
                                              worker::DocumentLinkParams{path},
                                              {.token = std::move(token)},
                                              suspect);
    if(!result.has_value()) {
        if(result.error().code == worker::dispatch_errc::worker_crashed) {
            session->quarantine.on_kind_crash(document_link_evidence,
                                              worker::death_of(result.error()));
        }
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "documentLink failed for {}: {}",
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The result carries byte offsets against the compiled buffer; a
    // didChange that landed during the await makes them describe text the
    // session no longer holds — the reply edge would map them onto the
    // edited buffer at wrong positions. The same staleness gates the query
    // ledger: a stale success must not launder the new content's evidence.
    if(session->generation != gen) {
        co_return std::vector<feature::DocumentLink>{};
    }
    bool was_active = session->quarantine.active();
    session->quarantine.on_kind_land(document_link_evidence);
    if(was_active && !session->quarantine.active()) {
        publish_recovered(session);
    }
    LOG_PERF("request",
             "kind=DocumentLink file={} wait_ms={:.2f} total_ms={:.2f}",
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

template <typename Params>
Compiler::RawResult Compiler::forward_interactive(std::uint8_t evidence,
                                                  llvm::StringRef label,
                                                  protocol::Position position,
                                                  std::shared_ptr<Session> session,
                                                  std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // This build compiles the same content the quarantine watches: while
    // the document is quarantined, only the recovery dispatch — the kind
    // holding the strikes, with the probe armed — may run. Anything else
    // is arbitrary work on proven-poisonous content. A refusal announces
    // the quarantine, or a completion-only client would never see it.
    if(session->quarantine.active() && !session->quarantine.recovery_kind(evidence)) {
        LOG_WARN("forward_interactive: {} is quarantined, refusing build", path);
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    auto flight = session->quarantine.begin_flight();

    // Takeoff snapshot for the pch_key write license (see
    // may_write_pch_key): this request runs concurrently with compiles and
    // holds no compiling token, so it is the easiest continuation to come
    // back stale after a disk/CDB change.
    auto epoch = session->dirty_epoch;

    Params wp;
    wp.file = path;
    wp.text = session->text;
    contexts.resolve_command(path, wp.directory, wp.arguments, ContextUse::Editor);
    contexts.append_suffix_include(session->path_id, wp.text);
    wp.config = workspace.config;

    ScopedTimer timer;
    if(!co_await ensure_deps(session, gen, epoch, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        LOG_WARN("forward_interactive: dependency preparation failed for {}", path);
        co_return kota::outcome_error(kota::ipc::Error{"Dependency preparation failed"});
    }
    // A PCH crash inside ensure_deps may have tipped the document into
    // quarantine after the entry gate: stop before dispatching the same
    // content again — that crash also spent any armed probe (it WAS the
    // attempt). A probe that predates this request's own evidence does not
    // excuse dispatching content that just proved poisonous.
    if(session->quarantine.active() && session->quarantine.grew(flight)) {
        session->quarantine.spend_probe();
        LOG_WARN("forward_interactive: {} quarantined during dependency prep", path);
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    auto wait_ms = timer.ms_f();

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    lsp::LineMap map(wp.text);
    wp.offset = clamped_offset(map, position);

    // The recovery license is re-taken here: the gate's answer may have
    // been spent by a concurrent recovery during the deps await. The guard
    // holds the spent probe across the dispatch and hands it back if the
    // coroutine unwinds (cancellation) or fails before any attempt ran —
    // an unavailable retry after a crashed first attempt keeps it spent,
    // the crash was the licensed attempt.
    bool recovery = session->quarantine.recovery_kind(evidence);
    if(session->quarantine.active() && !recovery) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await build_for(pool,
                                     *session,
                                     evidence,
                                     wp,
                                     worker::Priority::High,
                                     {.token = std::move(token)});
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "build (kind={}) failed for {}: {}",
                        label,
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    // The reply proves this kind on the DISPATCHED content answers; a
    // stale success must not launder evidence the new content recorded.
    // Leaving quarantine here clears the published diagnostic — no compile
    // runs to overwrite it.
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(evidence);
        if(was_active && !session->quarantine.active()) {
            publish_recovered(session);
        }
    }
    LOG_PERF("request",
             "kind={} file={} wait_ms={:.2f} total_ms={:.2f}",
             label,
             path,
             wait_ms,
             timer.ms_f());
    co_return std::move(result.value());
}

Compiler::RawResult Compiler::forward_completion(const protocol::Position& position,
                                                 std::shared_ptr<Session> session,
                                                 std::optional<kota::cancellation_token> token) {
    return forward_interactive<worker::CompletionParams>(completion_evidence,
                                                         "Completion",
                                                         position,
                                                         std::move(session),
                                                         std::move(token));
}

Compiler::RawResult
    Compiler::forward_signature_help(const protocol::Position& position,
                                     std::shared_ptr<Session> session,
                                     std::optional<kota::cancellation_token> token) {
    return forward_interactive<worker::SignatureHelpParams>(signature_help_evidence,
                                                            "SignatureHelp",
                                                            position,
                                                            std::move(session),
                                                            std::move(token));
}

Compiler::RawResult Compiler::forward_format(std::shared_ptr<Session> session,
                                             std::optional<protocol::Range> range,
                                             std::optional<kota::cancellation_token> token) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // Formatting runs no sema, but it is still this document's content on
    // a worker: while quarantined, only format-as-recovery may run, and a
    // refusal announces the quarantine.
    bool recovery = session->quarantine.recovery_kind(format_evidence);
    if(session->quarantine.active() && !recovery) {
        LOG_WARN("forward_format: {} is quarantined, refusing format", path);
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    worker::FormatParams wp;
    wp.file = path;
    wp.text = session->text;

    if(range) {
        lsp::LineMap map(wp.text);
        wp.range = {clamped_offset(map, range->start), clamped_offset(map, range->end)};
        if(wp.range.begin > wp.range.end) {
            co_return kota::outcome_error(
                kota::ipc::Error{kota::ipc::protocol::ErrorCode::InvalidParams,
                                 "Range start is after its end"});
        }
    }

    ScopedTimer timer;
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await build_for(pool,
                                     *session,
                                     format_evidence,
                                     wp,
                                     worker::Priority::High,
                                     {.token = std::move(token)});
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "format failed for {}: {}",
                        path,
                        result.error().message);
        }
        co_return kota::outcome_error(std::move(result.error()));
    }
    if(session->generation == gen) {
        bool was_active = session->quarantine.active();
        session->quarantine.on_kind_land(format_evidence);
        if(was_active && !session->quarantine.active()) {
            publish_recovered(session);
        }
    }
    LOG_PERF("request", "kind=Format file={} total_ms={:.2f}", path, timer.ms_f());
    co_return std::move(result.value());
}

}  // namespace clice
