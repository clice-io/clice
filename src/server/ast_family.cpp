#include "server/ast_family.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "command/argument_parser.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "sched/families/build_common.h"
#include "server/context_service.h"
#include "server/editor_context.h"
#include "server/position.h"
#include "support/anomaly.h"
#include "support/logging.h"
#include "support/timer.h"
#include "vfs/path.h"
#include "worker/protocol.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/codec/json.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "clang/Basic/Version.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

namespace {

/// What crashed, as the note words it: the work, and the feature it
/// pauses.
struct CrashSubject {
    llvm::StringRef work;
    llvm::StringRef paused;
};

CrashSubject crash_subject(std::uint8_t kind) {
    using Q = worker::QueryKind;
    if(kind >= evidence_kind(EvidenceKind::Count)) {
        switch(static_cast<Q>(kind - evidence_kind(EvidenceKind::Count))) {
            case Q::Hover: return {"computing hover for", "Hover is"};
            case Q::SemanticTokens:
                return {"computing semantic highlighting for", "Semantic highlighting is"};
            case Q::InlayHints: return {"computing inlay hints for", "Inlay hints are"};
            case Q::DocumentSymbol:
                return {"computing document symbols for", "Document symbols are"};
        }
        std::unreachable();
    }
    switch(static_cast<EvidenceKind>(kind)) {
        case EvidenceKind::Compile: return {"compiling", "Semantic features are"};
        case EvidenceKind::PCH:
            return {"building the precompiled preamble of", "Semantic features are"};
        case EvidenceKind::PCM: return {"building a module imported by", "The module is"};
        case EvidenceKind::DocumentLink:
            return {"computing document links for", "Document links are"};
        case EvidenceKind::FoldingRange:
            return {"computing folding ranges for", "Folding ranges are"};
        case EvidenceKind::CodeAction: return {"computing code actions for", "Code actions are"};
        case EvidenceKind::Completion: return {"completing code in", "Code completion is"};
        case EvidenceKind::SignatureHelp:
            return {"computing signature help in", "Signature help is"};
        case EvidenceKind::Format: return {"formatting", "Formatting is"};
        case EvidenceKind::Count: break;
    }
    std::unreachable();
}

}  // namespace

void append_crash_notes(const Session& session, std::vector<protocol::Diagnostic>& diagnostics) {
    if(session.closed) {
        return;
    }
    for(auto& note: session.quarantine->notes()) {
        auto subject = crash_subject(note.kind);
        llvm::StringRef retry =
            note.save_only
                ? "until you save this file"
                : "until this file or a header it includes changes, or until you save it";
        auto repeats =
            note.strikes > 1 ? std::format(" {} times in a row", note.strikes) : std::string();
        protocol::Diagnostic diagnostic;
        diagnostic.range = protocol::Range{
            .start = protocol::Position{.line = 0, .character = 0},
            .end = protocol::Position{.line = 0, .character = 0},
        };
        diagnostic.severity = protocol::DiagnosticSeverity::Warning;
        diagnostic.source = "clice";
        diagnostic.message =
            std::format("clice's worker crashed{} while {} this file ({}). {} paused here {}.",
                        repeats,
                        subject.work,
                        note.cause,
                        subject.paused,
                        retry);
        diagnostics.push_back(std::move(diagnostic));
    }
}

/// The compile's diagnostics behind the ones its PCH's build raised in the
/// preamble, which the parse consuming the PCH never raises again — those
/// of the command line it does, and they appear once. Files with one
/// preamble share the PCH: related information the build placed in its
/// own main file moves to `path`.
static kota::codec::RawValue with_preamble(kota::codec::RawValue diagnostics,
                                           const index::TUIndex& preamble,
                                           llvm::StringRef path) {
    std::vector<protocol::Diagnostic> merged;
    [[maybe_unused]] auto status =
        kota::codec::json::from_string<kota::ipc::lsp_config>(preamble.preamble_diagnostics(),
                                                              merged);
    if(merged.empty()) {
        return diagnostics;
    }
    auto builder = feature::to_uri(preamble.path(preamble.path_count() - 1));
    auto uri = feature::to_uri(path);
    for(auto& diagnostic: merged) {
        if(!diagnostic.related_information) {
            continue;
        }
        for(auto& related: *diagnostic.related_information) {
            if(related.location.uri == builder) {
                related.location.uri = uri;
            }
        }
    }
    std::vector<protocol::Diagnostic> own;
    if(!diagnostics.empty()) {
        status = kota::codec::json::from_string<kota::ipc::lsp_config>(diagnostics.data, own);
    }
    llvm::StringSet<> raised;
    for(auto& diagnostic: own) {
        if(auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(diagnostic)) {
            raised.insert(*json);
        }
    }
    std::erase_if(merged, [&](const protocol::Diagnostic& diagnostic) {
        auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(diagnostic);
        return json && raised.contains(*json);
    });
    std::ranges::move(own, std::back_inserter(merged));
    auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(merged);
    return kota::codec::RawValue{json ? std::move(*json) : "[]"};
}

ASTFamily::PCHPlan ASTFamily::plan_pch(Fid path_id,
                                       llvm::StringRef text,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments,
                                       const SynthesizedContext* synthesized) {
    auto path = project.file_table.resolve(path_id);
    auto bound = compute_preamble_bound(text);
    if(bound == 0 && !synthesized) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts land here too: they borrow
        // a command but inject nothing.
        return {};
    }

    // With a synthesized prefix, the PCH is worth building even at
    // bound == 0: the -include'd prefix is processed via the predefines
    // buffer and lands in the PCH, so the (potentially huge) prefix is not
    // re-parsed on every edit. The -include flag is part of the
    // canonicalized arguments below, and the prefix's name hashes its
    // content — through the fragments it includes, the whole chain's — so
    // the key tracks prefix changes automatically.

    // Key the PCH by preamble text plus the frontend-relevant compile flags,
    // so files with the same preamble text but different flags (-D, -I, -std)
    // produce separate PCHs.  The source file path stays out of the key so
    // files with identical preambles share one PCH — but its DIRECTORY (and
    // the working directory) must stay in: quote includes and relative paths
    // resolve against them, so equal preamble text in different directories
    // can mean different content.  The clang version guards against reusing
    // blobs a newer bundled clang would reject, and the build configuration
    // keeps the blob with the library that records its dependencies:
    // shared across configurations, one could rebuild it while another's
    // records still vouched for the old content.
    auto preamble_text = text.substr(0, bound);
    auto pch_key = cache_key({clang::getClangFullVersion(),
                              pch_input_check,
                              project.build.active_configuration(),
                              directory,
                              path::parent_path(path),
                              preamble_text,
                              canonicalize(arguments, ArgsProfile::Frontend)});
    // The text first: freshness checks every dependency of the key.
    if(!is_preamble_complete(text, bound) && !pch.fresh(pch_key)) {
        // Preamble incomplete (user still typing) and nothing fresh to
        // adopt under the new key: defer the rebuild, keep using the
        // previously adopted PCH while its artifact is still available.
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        PCHPlan plan{.verdict = PCHPlan::Verdict::Defer};
        if(auto previous = projections.projection(path_id); previous && previous->pch_key) {
            auto it = project.pch_cache.find(*previous->pch_key);
            if(it != project.pch_cache.end() && !it->second.path.empty()) {
                plan.previous = previous->pch_key;
            }
        }
        return plan;
    }
    return {
        .verdict = PCHPlan::Verdict::Acquire,
        .request =
            {
                      .pch_key = std::move(pch_key),
                      .file = std::string(path),
                      .directory = directory,
                      .arguments = arguments,
                      .content = std::string(text),
                      .preamble_bound = bound,
                      .synthesized = synthesized ? synthesized->files : SynthesizedFiles{},
                      },
    };
}

ASTFamily::ASTFamily(Project& project,
                     EditorContext& contexts,
                     TaskGraph& graph,
                     PCMFamily& pcm,
                     PCHFamily& pch,
                     WorkerPool& pool,
                     SessionStore& sessions) :
    project(project), contexts(contexts), graph(graph), pcm(pcm), pch(pch), pool(pool),
    sessions(sessions) {}

void ASTFamily::register_runner() {
    graph.register_family(Family::AST, [this](RoundContext& ctx, NodeId id) {
        return run(ctx, Fid{static_cast<std::uint32_t>(id.key)});
    });
}

void ASTFamily::record_crash(const std::shared_ptr<Session>& session,
                             std::uint8_t kind,
                             const kota::ipc::Error& error) {
    LOG_WARN("{} crashed a worker while serving {}: {}",
             project.file_table.resolve(session->path_id),
             crash_subject(kind).work,
             error.message);
    session->quarantine->on_crash(kind,
                                  worker::death_of(error),
                                  error.message,
                                  Quarantine::Clock::now());
    // A silent first crash leaves the published state alone (see
    // Quarantine invariant 4).
    if(!session->quarantine->shows(kind)) {
        return;
    }
    // A closed session publishes nothing; a reopen sharing its records
    // shows the note.
    if(session->closed) {
        if(auto open = sessions.find(session->path_id);
           open && open->quarantine == session->quarantine) {
            republish(open);
        }
        return;
    }
    // A barred compile leaves no AST behind the old diagnostics: they go,
    // and the note says why.
    auto previous = projections.projection(session->path_id);
    if(previous && previous->output.has_value() &&
       (kind == evidence_kind(EvidenceKind::Compile) || kind == evidence_kind(EvidenceKind::PCH))) {
        projections.set_output(session->path_id,
                               CompileOutput{
                                   .version = std::nullopt,
                                   .source = previous->output->source,
                                   .diagnostics = kota::codec::RawValue{},
                                   .line_limit = std::nullopt,
                               });
    }
    republish(session);
}

void ASTFamily::republish(const std::shared_ptr<Session>& session) {
    if(auto previous = projections.projection(session->path_id);
       !previous || !previous->output.has_value()) {
        projections.set_output(session->path_id,
                               CompileOutput{
                                   .version = std::nullopt,
                                   .source = CommandSource::CDBExact,
                                   .diagnostics = kota::codec::RawValue{},
                                   .line_limit = std::nullopt,
                               });
    }
    on_output.emit(session);
}

bool ASTFamily::compile_barred(const Session& session) {
    auto now = Quarantine::Clock::now();
    return session.quarantine->barred(evidence_kind(EvidenceKind::Compile), now) ||
           session.quarantine->barred(evidence_kind(EvidenceKind::PCH), now);
}

void ASTFamily::saved(Session& session) {
    session.quarantine->on_save();
    // The modules it is and imports retry with it: a crashed build is
    // refused until a consumer holds a license (see depend_modules). A
    // module crash leaves the importer's compile standing, so only a fresh
    // round asks for the module again.
    if(session.quarantine->crashed(evidence_kind(EvidenceKind::PCM))) {
        invalidate(session.path_id);
    }
    pcm.forgive(session.path_id);
    for(auto dep: graph.dependencies(node(session.path_id))) {
        if(dep.family == Family::PCM) {
            pcm.forgive(Fid{static_cast<std::uint32_t>(dep.key)});
        }
    }
}

void ASTFamily::publish_output(const std::shared_ptr<Session>& session, CompileOutput output) {
    projections.set_output(session->path_id, std::move(output));
    on_output.emit(session);
}

bool ASTFamily::is_stale(const Session& session) {
    // No early return: the changes one check finds cascade together.
    bool stale = false;
    auto it = projections.entries.find(session.path_id);
    if(it != projections.entries.end() && it->second.deps.has_value()) {
        stale = deps_changed(project.file_table, *it->second.deps);
    }

    // Chain files of a header context are embedded in the synthesized
    // preamble, invisible to the deps snapshot — check them explicitly.
    if(auto* header_context = contexts.header_context(session.path_id)) {
        stale = deps_changed(project.file_table, header_context->deps) || stale;
    }

    // Check PCH staleness via the projection's pch_key.
    auto projection = projections.projection(session.path_id);
    if(projection && projection->pch_key.has_value()) {
        auto pch_it = project.pch_cache.find(*projection->pch_key);
        if(pch_it != project.pch_cache.end()) {
            stale = deps_changed(project.file_table, pch_it->second.deps) || stale;
        }
    }

    return stale;
}

void ASTFamily::closure(Fid path_id, llvm::SmallVectorImpl<Fid>& files) {
    auto add = [&](const DepsSnapshot& deps) {
        for(auto& dep: deps) {
            files.push_back(dep.path_id);
        }
    };
    if(auto it = projections.entries.find(path_id);
       it != projections.entries.end() && it->second.deps) {
        add(*it->second.deps);
    }
    if(auto* header_context = contexts.header_context(path_id)) {
        add(header_context->deps);
    }
    if(auto projection = projections.projection(path_id); projection && projection->pch_key) {
        if(auto it = project.pch_cache.find(*projection->pch_key); it != project.pch_cache.end()) {
            add(it->second.deps);
        }
    }
    // A module's snapshot covers the modules it imports in turn.
    for(auto dep: graph.dependencies(node(path_id))) {
        if(dep.family != Family::PCM) {
            continue;
        }
        Fid module{static_cast<std::uint32_t>(dep.key)};
        if(auto it = project.pcm_cache.find(module); it != project.pcm_cache.end()) {
            files.push_back(module);
            add(it->second.deps);
        }
    }
}

void ASTFamily::touch(Fid path_id) {
    auto& entry = projections.entries[path_id];
    entry.current = false;
    entry.epoch += 1;
}

void ASTFamily::supersede(Fid path_id) {
    touch(path_id);
    graph.update(node(path_id));
    // Not a wire cancel: the notification flips the compile's stop flag
    // and the round still observes its real reply (crash accounting
    // depends on it — contract 2). FIFO order puts it ahead of any
    // replacement Compile, which can only enter the pipe after this
    // round lands.
    if(graph.is_compiling(node(path_id))) {
        pool.notify_stateful(
            path_id.raw,
            worker::CancelCompileParams{std::string(project.file_table.resolve(path_id))});
    }
}

void ASTFamily::invalidate(Fid path_id) {
    touch(path_id);
    // The in-flight round's token fires, but its parse keeps running: the
    // buffer is unchanged, so the product is worth publishing as bounded
    // staleness before the round reports Stale.
    graph.update(node(path_id));
}

void ASTFamily::drop(Fid path_id) {
    graph.update(node(path_id));
    projections.entries.erase(path_id);
    pcm.forget_buffer(path_id);
}

void ASTFamily::switch_identity(Session& session) {
    // Invalidate any in-flight compile: without the bump it would pass
    // its landing validity check and publish results for the superseded
    // identity.
    session.generation += 1;
    session.trial_done = false;
    session.quarantine->on_change(Quarantine::Clock::now());
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
    sessions.for_each([&](Fid path_id, const Session&) {
        if(graph.is_compiling(node(path_id))) {
            pool.notify_stateful(
                path_id.raw,
                worker::CancelCompileParams{std::string(project.file_table.resolve(path_id))});
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

    // A document whose compile keeps killing workers waits for a change
    // or a save instead of feeding the same content to one more worker;
    // the crash note was published when the crash was recorded.
    if(compile_barred(*session)) {
        LOG_DEBUG("ensure_compiled: {} is barred by a crash", project.file_table.resolve(path_id));
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
        // lazy twin of the background ticks. The document recompiles now,
        // whether or not the dependency graph knows the edge (a macro
        // include). The handler re-resolves the session by path_id; no
        // suspension separates it from this frame, so it finds the same
        // open session this coroutine holds.
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

kota::task<bool> ASTFamily::depend_modules(RoundContext& ctx,
                                           const std::shared_ptr<Session>& session,
                                           const Resolution& resolution,
                                           llvm::StringRef directory,
                                           const std::vector<std::string>& arguments,
                                           llvm::StringRef text) {
    auto path_id = session->path_id;
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
    // keeps the edges honest across CDB, buffer and import changes, and
    // the empty truth of a document that can import nothing is published
    // too: a durable import edge earned earlier must stop cascading here.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto deps = co_await pcm.direct_deps(path_id,
                                         resolution,
                                         argv,
                                         directory,
                                         std::optional<llvm::StringRef>(text));
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
        session->quarantine->on_land(evidence_kind(EvidenceKind::PCM));
        co_return true;
    }

    // A module whose build crashed a worker is refused to every importer
    // until one holds a license to retry it: a change to its inputs, or a
    // save (see Quarantine).
    auto kind = evidence_kind(EvidenceKind::PCM);
    bool licensed = session->quarantine->crashed(kind) &&
                    !session->quarantine->barred(kind, Quarantine::Clock::now());
    Quarantine::Attempt license(*session->quarantine, kind);
    bool crashed = false;

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
            if(auto* crash = pcm.crashed(dep)) {
                if(!licensed) {
                    // Booked once: a round on unchanged inputs is no new
                    // crash.
                    if(!session->quarantine->crashed(kind)) {
                        record_crash(session, kind, *crash);
                    }
                    crashed = true;
                    // Still an input: a later build of it, by an importer
                    // holding a license, must re-dirty this document.
                    ctx.reference({Family::PCM, dep.raw});
                    continue;
                }
                pcm.forgive(dep);
            }
            switch(co_await ctx.depend({Family::PCM, dep.raw})) {
                case DependResult::Ready: break;
                case DependResult::Failed:
                    LOG_INFO("Import {} of {} failed to build; the parse reports it",
                             project.file_table.resolve(dep),
                             project.file_table.resolve(path_id));
                    if(auto* crash = pcm.crashed(dep)) {
                        record_crash(session, kind, *crash);
                        crashed = true;
                        licensed = false;
                    }
                    break;
                case DependResult::Cancelled: co_return false;
            }
        }
    }
    if(!crashed) {
        session->quarantine->on_land(kind);
    }
    co_return true;
}

kota::task<RoundOutcome> ASTFamily::run(RoundContext& ctx, Fid path_id) {
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
    // that just crashed a worker must not reach one more.
    if(compile_barred(*session)) {
        co_return RoundOutcome::Failed;
    }

    ScopedTimer timer;
    auto file_path = std::string(project.file_table.resolve(path_id));

    LOG_INFO("compile round: starting path_id={} gen={}", path_id, gen);

    // A round past the gate on a crashed compile or preamble is its retry,
    // and holds the license until its outcome (Quarantine invariant 2).
    auto compile_kind = evidence_kind(EvidenceKind::Compile);
    auto pch_kind = evidence_kind(EvidenceKind::PCH);
    Quarantine::Attempt compile_attempt(*session->quarantine, compile_kind);
    bool pch_licensed = session->quarantine->crashed(pch_kind);
    Quarantine::Attempt pch_attempt(*session->quarantine, pch_kind);

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
        params.workspace = project.config.workspace_root.str();
        auto resolution = contexts.resolve_command(path_id, params.directory, params.arguments);
        auto source = resolution.source;
        auto* synthesized = resolution.synthesized.get();

        // The line the appended suffix #include lands on — anything at or
        // past it is phantom text the user cannot see.
        std::optional<std::uint32_t> suffix_line_limit;
        auto* header_context = contexts.header_context(path_id);
        if(synthesized) {
            params.synthesized = synthesized->files;
            if(!synthesized->suffix.empty()) {
                auto newlines = std::ranges::count(params.text, '\n');
                suffix_line_limit =
                    static_cast<std::uint32_t>(newlines + (params.text.ends_with('\n') ? 0 : 1));
            }
            synthesized->append_suffix_include(params.text);
        }

        // Whether this round is the self-containment probe: a header
        // deliberately compiled without its includer prefix to see if it
        // stands alone. Decided here, where resolve_command chose to omit
        // the prefix; the landing gates what the probe may write.
        bool trial_round = attempt == 0 && !session->trial_done && header_context &&
                           !header_context->synthesized &&
                           contexts.commands.header_mode(path_id) == HeaderMode::Unknown;

        if(!co_await depend_modules(ctx,
                                    session,
                                    resolution,
                                    params.directory,
                                    params.arguments,
                                    params.text)) {
            co_return RoundOutcome::Stale;
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before PCH for {}", file_path);
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
            auto plan =
                plan_pch(path_id, params.text, params.directory, params.arguments, synthesized);
            switch(plan.verdict) {
                // No preamble left to crash on.
                case PCHPlan::Verdict::None: session->quarantine->on_land(pch_kind); break;
                case PCHPlan::Verdict::Defer: adopted_pch = plan.previous; break;
                case PCHPlan::Verdict::Acquire: {
                    auto pch_key = plan.request.pch_key;
                    // A preamble whose build crashed a worker — for this
                    // document or another sharing it — is refused until
                    // this one holds a license to retry it.
                    if(auto* crash = pch.crashed(pch_key)) {
                        if(!pch_licensed) {
                            record_crash(session, pch_kind, *crash);
                            co_return RoundOutcome::Failed;
                        }
                        pch.forgive(pch_key);
                    }
                    auto dep = pch.prepare(std::move(plan.request));
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
                                // Adopting a proven-good artifact clears
                                // the session's PCH record as surely as
                                // building one — but only its own; every
                                // consumer clears for itself.
                                session->quarantine->on_land(pch_kind);
                            }
                            break;
                        case DependResult::Failed:
                            // The build itself crashed a worker: this round
                            // stops before the parse, which would feed the
                            // same preamble to one more worker.
                            if(auto* crash = pch.crashed(pch_key)) {
                                record_crash(session, pch_kind, *crash);
                                co_return RoundOutcome::Failed;
                            }
                            // A build that failed without a crash answered:
                            // the preamble no longer crashes.
                            if(session->generation == gen && ctx.current()) {
                                session->quarantine->on_land(pch_kind);
                            }
                            break;
                        case DependResult::Cancelled: co_return RoundOutcome::Stale;
                    }
                    break;
                }
            }
        }
        if(adopted_pch.has_value()) {
            if(auto pch_it = project.pch_cache.find(*adopted_pch);
               pch_it != project.pch_cache.end() && !pch_it->second.path.empty()) {
                params.pch = {pch_it->second.path, pch_it->second.bound};
            } else {
                adopted_pch.reset();
            }
        }

        // Fill all available PCM paths, excluding the file's own PCM
        // to avoid "multiple module declarations".
        project.fill_pcm_deps(params.pcms, path_id);

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded before send for {}", file_path);
            co_return RoundOutcome::Stale;
        }

        // Seed the worker's inactive-region state from the PCH's preamble:
        // the conditional stack it left open (a #if cut by the bound) and
        // the regions of the preamble share itself. Copy the state out:
        // concurrent compiles can insert into pch_cache across the await
        // below and rehash the map from under a held pointer.
        std::shared_ptr<index::TUIndex> preamble_state;
        if(adopted_pch.has_value()) {
            preamble_state = pch.preamble_state(*adopted_pch);
        }
        if(preamble_state) {
            auto regions = preamble_state->inactive_regions();
            params.preamble_inactive_regions.assign(regions.begin(), regions.end());
            auto conditionals = preamble_state->open_conditionals();
            params.open_conditionals.assign(conditionals.begin(), conditionals.end());
        }

        // The send deliberately carries no token: the master must observe
        // the request's real outcome — the crash accounting below depends
        // on it (contract 2). A supersede interrupts the worker with a
        // CancelCompile notification instead (see supersede/stop), and
        // the stale reply is discarded at the validity gate below. A death
        // that is not this compile's doing resends it once — the attempt
        // only stops early once the buffer moved on, since the next round
        // compiles that.
        //
        // Crash accounting runs even for superseded rounds: the crash came
        // from content this document dispatched, and skipping it would let a
        // poison file dodge its bar by being edited between dispatch and the
        // crash response (contract 12).
        //
        // A death while consuming a prebuilt pair may be the pair's fault:
        // deep corruption aborts the AST reader (report_fatal_error in the
        // bitstream reader) before any diagnostic can anchor, so the
        // pch_suspect gate below never gets a say. The first such death
        // retracts the pair and the round respawns on a rebuilt one; a
        // death on the rebuilt pair is the document's own.
        bool consuming_pch = adopted_pch.has_value();
        bool pch_crashed = false;
        auto result = co_await deliver(
            pool,
            true,
            [&]() -> RequestResult<worker::CompileParams> {
                if(session->generation != gen) {
                    co_return kota::outcome_error(
                        kota::ipc::Error{worker::dispatch_errc::cancelled, "Compile superseded"});
                }
                co_return co_await pool.send_stateful(path_id.raw, params);
            },
            [&](const kota::ipc::Error& error) {
                if(consuming_pch && session->crashed_pch != *adopted_pch) {
                    pch_crashed = true;
                } else {
                    record_crash(session, compile_kind, error);
                }
            });

        if(pch_crashed) {
            LOG_WARN("Compile crashed consuming PCH pair {} for {}; retracting the pair",
                     *adopted_pch,
                     file_path);
            session->crashed_pch = *adopted_pch;
            pch.blame(*adopted_pch);
            co_return RoundOutcome::Stale;
        }

        if(session->generation != gen) {
            LOG_INFO("compile round: superseded reply for {}", file_path);
            co_return RoundOutcome::Stale;
        }

        if(!result.has_value()) {
            if(worker::is_operational_error(result.error())) {
                LOG_WARN("Compile did not complete for {}: {}", file_path, result.error().message);
            } else {
                // The worker accepts arbitrary user code; a non-operational
                // failure at this layer is IPC/worker breakage, never a
                // user-code problem.
                LOG_ANOMALY(CompileFail,
                            "Compile failed for {}: {}",
                            file_path,
                            result.error().message);
            }
            // Short of a crash of its own (published as it was recorded),
            // the document keeps what it showed: a worker outage or a
            // second death of somebody else's doing says nothing about its
            // code.
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
        if(result.value().pch_suspect && consuming_pch) {
            LOG_WARN("Compile blamed PCH pair {} for {}; retracting the pair",
                     *adopted_pch,
                     file_path);
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
                     file_path,
                     static_cast<int>(result.value().status));
            // The projection stays non-current: the next request
            // recompiles instead of trusting the phantom product. Publish
            // the honest gap — versionless empty diagnostics rather than a
            // stale list posing as current.
            publish_output(session,
                           CompileOutput{
                               .version = std::nullopt,
                               .source = source,
                               .diagnostics = kota::codec::RawValue{},
                               .line_limit = suffix_line_limit,
                           });
            co_return RoundOutcome::Failed;
        }

        // A probe invalidated mid-flight is discarded whole: its verdict is
        // a conditional write like the projection's current flag (dispatch
        // reset trial_done and the header mode for the recompile to
        // re-earn), and its diagnostics come from a compile deliberately
        // run without includer context — they are never published,
        // including on this path.
        if(trial_round && !ctx.current()) {
            LOG_INFO("Discarding invalidated self-containment probe for {}", file_path);
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
                    kota::codec::json::from_string<kota::ipc::lsp_config>(
                        result.value().diagnostics.data,
                        diagnostics);
            }
            session->trial_done = true;
            contexts.commands.record_header_mode(path_id, HeaderMode::SelfContained);

            if(indicates_missing_context(diagnostics)) {
                LOG_INFO("Header {} needs includer context, re-compiling with prefix", file_path);
                // Scored on the buffer: a restart keeps it only for the
                // same text on disk.
                contexts.commands.record_header_mode(path_id,
                                                     HeaderMode::NeedsContext,
                                                     session->hash);
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
        if(current && consuming_pch && !result.value().pch_suspect) {
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
        auto& diagnostics = result.value().diagnostics;
        next->output = CompileOutput{
            .version = session->version,
            .source = source,
            .diagnostics = preamble_state && preamble_state->matches_prefix(params.text)
                               ? with_preamble(std::move(diagnostics), *preamble_state, file_path)
                               : std::move(diagnostics),
            .line_limit = suffix_line_limit,
        };

        auto& entry = projections.entries[path_id];
        entry.projection = std::move(next);
        entry.deps =
            capture_deps_snapshot(project.file_table, result.value().deps, result.value().build_at);
        entry.current = current;
        session->quarantine->on_land(compile_kind);
        session->crashed_pch.clear();
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

kota::task<std::optional<std::string>>
    ASTFamily::ensure_pch(const std::shared_ptr<Session>& session,
                          llvm::StringRef text,
                          std::uint64_t license_generation,
                          std::uint64_t license_epoch,
                          const std::string& directory,
                          const std::vector<std::string>& arguments,
                          const SynthesizedContext* synthesized) {
    auto path_id = session->path_id;
    auto license = [&] {
        return session->generation == license_generation &&
               projections.epoch(path_id) == license_epoch;
    };

    auto plan = plan_pch(path_id, text, directory, arguments, synthesized);
    switch(plan.verdict) {
        case PCHPlan::Verdict::None:
            if(license()) {
                projections.set_pch_key(path_id, std::nullopt);
                session->quarantine->on_land(evidence_kind(EvidenceKind::PCH));
            }
            co_return std::nullopt;
        // The adopted PCH may by now belong to a newer buffer than a
        // stale request's; without the license it cannot tell.
        case PCHPlan::Verdict::Defer: co_return license() ? plan.previous : std::nullopt;
        case PCHPlan::Verdict::Acquire: break;
    }
    auto pch_key = plan.request.pch_key;

    // A preamble that crashed a worker bars the document's content builds
    // (the caller checks); only a compile round's license forgives it.
    auto pch_kind = evidence_kind(EvidenceKind::PCH);
    if(auto* crash = pch.crashed(pch_key)) {
        // Booked once: a request on unchanged inputs is no new crash.
        if(!session->quarantine->crashed(pch_kind)) {
            record_crash(session, pch_kind, *crash);
        }
        co_return std::nullopt;
    }
    if(co_await pch.acquire(std::move(plan.request)) != PCHFamily::Outcome::Ready) {
        if(auto* crash = pch.crashed(pch_key)) {
            record_crash(session, pch_kind, *crash);
        }
        co_return std::nullopt;
    }

    // Adoption is gated on the round outcome, never on leftover cache
    // paths, and on this request's own license: a supersede or a
    // Lost-type invalidation while we waited means the resolved command
    // may describe nothing — neither the key write nor the evidence
    // wash belongs to this request anymore. The request itself still
    // compiles against the artifact: it was planned from the text it sends.
    if(license()) {
        projections.set_pch_key(path_id, pch_key);
        // Adopting a proven-good artifact clears the session's PCH record as
        // surely as building one — but only its own; every consumer clears
        // for itself.
        session->quarantine->on_land(pch_kind);
    }
    co_return pch_key;
}

kota::task<bool> ASTFamily::prepare_stateless_inputs(const Ticket& ticket,
                                                     llvm::StringRef text,
                                                     const std::string& directory,
                                                     const std::vector<std::string>& arguments,
                                                     const Resolution& resolution,
                                                     StatelessInputs& inputs) {
    auto* synthesized = resolution.synthesized.get();
    auto& session = ticket.session;
    auto path_id = session->path_id;
    auto license_epoch = projections.epoch(path_id);

    // The scan runs under the request's command with the same text the
    // dispatch will compile — buffer plus any appended suffix include — so an
    // unsaved `import m;` (or one inside a contextual header's suffix)
    // builds its PCM before the parse needs it.
    llvm::SmallVector<const char*, 32> argv;
    argv.reserve(arguments.size());
    for(auto& arg: arguments) {
        argv.push_back(arg.c_str());
    }
    auto scan_text = text.str();
    if(synthesized) {
        synthesized->append_suffix_include(scan_text);
    }
    if(!co_await pcm.prepare_deps(path_id, resolution, argv, directory, scan_text)) {
        co_return false;
    }

    if(readonly != ReadonlyMode::On) {
        auto pch_key = co_await ensure_pch(session,
                                           text,
                                           ticket.generation,
                                           license_epoch,
                                           directory,
                                           arguments,
                                           synthesized);
        if(pch_key) {
            if(auto pch_it = project.pch_cache.find(*pch_key); pch_it != project.pch_cache.end()) {
                inputs.pch = {pch_it->second.path, pch_it->second.bound};
            }
        }
    }

    // Fill all available PCM paths, excluding the file's own PCM
    // to avoid "multiple module declarations".
    project.fill_pcm_deps(inputs.pcms, path_id);

    co_return true;
}

}  // namespace clice
