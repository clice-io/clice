#include "server/compiler/compiler.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "command/argument_parser.h"
#include "index/preamble_state.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/protocol/extension.h"
#include "server/protocol/worker.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"

#include "kota/async/async.h"
#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/uri.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"
#include "clang/Basic/Version.h"

namespace clice {

namespace lsp = kota::ipc::lsp;
using serde_raw = kota::codec::RawValue;

/// Render hash-input fragments into an unambiguous byte stream (length-
/// prefixed, so embedded NULs cannot create colliding splits) and return
/// the 32-hex xxh3_128bits cache key.
///
/// FIXME: this concatenates all parts (including the preamble text, which
/// can be 10-100 KB) into a temporary std::string before hashing.  Use an
/// incremental xxh3 hasher to feed each StringRef directly and avoid the
/// large allocation on the ensure_pch hot path.
static std::string cache_key(std::initializer_list<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

/// RAII completion of an in-flight PCH build registration: wakes waiters
/// and clears the building marker on every exit path — crucially also when
/// the coroutine is cancelled and its frame unwinds at a suspension point,
/// which would otherwise leave waiters suspended on the event forever.
struct BuildingGuard {
    Workspace& workspace;
    llvm::StringRef key;
    std::shared_ptr<kota::event> completion;

    ~BuildingGuard() {
        // Reset only our own registration: the entry may have been
        // re-registered by a newer build in the meantime.
        if(auto it = workspace.pch_cache.find(key);
           it != workspace.pch_cache.end() && it->second.building == completion) {
            it->second.building.reset();
        }
        completion->set();
    }
};

/// A PCH/PCM build failure is expected when the worker reported user-code
/// errors or the dispatch failed for an operational reason (memory-pressure
/// preemption, crash/restart window); anything else is clice breakage and is
/// reported as an anomaly.
static bool expected_build_failure(const auto& result) {
    return result.has_value() ? result.value().has_user_errors
                              : worker::is_operational_error(result.error());
}

/// The error text of a failed build: the worker's message when it responded,
/// the dispatch error otherwise.
const static std::string& build_failure_message(const auto& result) {
    return result.has_value() ? result.value().error : result.error().message;
}

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
        "compiling this file crashed the language server worker {} times; " "the file is quarantined until it is edited",
        crashes);
    auto json = kota::codec::json::to_json<kota::ipc::lsp_config>(diagnostics);
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
static kota::ipc::RequestResult<Params> send_stateless_retrying(WorkerPool& pool,
                                                                Params params,
                                                                OnCrash on_crash) {
    auto result = co_await pool.send_stateless(params);
    if(!result.has_value() && result.error().code == worker::dispatch_errc::worker_crashed) {
        on_crash(result.error());
        result = co_await pool.send_stateless(params);
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
static kota::ipc::RequestResult<Params>
    build_for(WorkerPool& pool, Session& session, std::uint8_t kind, Params params) {
    return send_stateless_retrying(pool,
                                   std::move(params),
                                   [&session, kind](const kota::ipc::protocol::Error& error) {
                                       session.quarantine.on_kind_crash(kind,
                                                                        worker::death_of(error));
                                   });
}

/// Evidence-kind discriminators for Quarantine's per-kind ledgers. Queries
/// and stateless builds share one space, offset so they cannot collide;
/// document links have no QueryKind and get their own slot.
constexpr std::uint8_t evidence_kind(worker::QueryKind kind) {
    return static_cast<std::uint8_t>(kind);
}

constexpr std::uint8_t evidence_kind(worker::BuildKind kind) {
    return 0x40 + static_cast<std::uint8_t>(kind);
}

constexpr inline std::uint8_t document_link_evidence = 0x20;

/// Clamp a client-supplied position to the document, following LSP
/// semantics: a character beyond the line length defaults to the line end,
/// a line beyond the document defaults to the end of the content.
static lsp::LineMap::Offset clamped_offset(const lsp::LineMap& map,
                                           const protocol::Position& position) {
    if(auto offset = map.to_offset(position)) {
        return *offset;
    }
    auto starts = map.line_starts();
    if(position.line >= starts.size()) {
        return static_cast<lsp::LineMap::Offset>(map.content().size());
    }
    return map.line_bounds(starts[position.line]).end;
}

Compiler::Compiler(kota::event_loop& loop,
                   Workspace& workspace,
                   ContextResolver& contexts,
                   WorkerPool& pool) :
    loop(loop), workspace(workspace), contexts(contexts), pool(pool) {}

Compiler::~Compiler() {
    workspace.cancel_all();
}

kota::task<> Compiler::stop() {
    compile_tasks.cancel();
    co_await compile_tasks.join();

    // Requests have unwound and released their interest; now tear down the
    // module compile graph's own unit tasks.
    if(workspace.compile_graph) {
        co_await workspace.compile_graph->shutdown();
    }
}

void Compiler::init_compile_graph() {
    if(workspace.path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = workspace.path_pool.resolve(path_id);
        std::vector<std::string> rule_append, rule_remove;
        workspace.config.match_rules(file_path, rule_append, rule_remove);
        auto results =
            workspace.cdb.lookup(file_path, {.remove = rule_remove, .append = rule_append});
        if(results.empty())
            return {};
        workspace.toolchain.resolve_or_warn(results[0]);

        auto& cmd = results[0];
        auto scan_result = scan_precise(cmd.to_argv(), cmd.resolved.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = workspace.dep_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        // Module implementation units implicitly depend on their interface unit.
        if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
            auto mod_ids = workspace.dep_graph.lookup_module(scan_result.module_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        return deps;
    };

    // Dispatch: sends BuildPCM request to a stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> kota::task<bool> {
        auto mod_it = workspace.path_to_module.find(path_id);
        if(mod_it == workspace.path_to_module.end())
            co_return false;

        // Copy out of the map before any suspension below: while a PCM build
        // is awaited, a concurrent didSave can insert into (or erase from)
        // path_to_module, rehashing the DenseMap and invalidating mod_it.
        auto module_name = mod_it->second;

        auto file_path = std::string(workspace.path_pool.resolve(path_id));

        worker::BuildParams bp;
        bp.kind = worker::BuildKind::BuildPCM;
        bp.file = file_path;
        contexts.resolve_command(file_path, bp.directory, bp.arguments);

        if(!workspace.store) {
            LOG_WARN("BuildPCM skipped for module {}: cache store is unavailable", module_name);
            co_return false;
        }

        // Deterministic content-addressed PCM key over the source path and
        // the frontend-relevant subset of the compile flags.
        auto safe_module_name = module_name;
        std::ranges::replace(safe_module_name, ':', '-');
        auto pcm_key = std::format("{}-{}",
                                   safe_module_name,
                                   cache_key({clang::getClangFullVersion(),
                                              bp.directory,
                                              file_path,
                                              canonicalize(bp.arguments, ArgsProfile::Frontend)}));

        // Check if cached PCM is still valid.
        llvm::StringRef pcm_miss = "no_entry";
        if(auto pcm_it = workspace.pcm_cache.find(path_id); pcm_it != workspace.pcm_cache.end()) {
            if(pcm_it->second.key != pcm_key) {
                pcm_miss = "key_changed";
            } else if(!workspace.store->lookup("pcm", pcm_key)) {
                pcm_miss = "evicted";
            } else if(deps_changed(workspace.path_pool, pcm_it->second.deps)) {
                pcm_miss = "deps_changed";
            } else {
                workspace.pcm_paths[path_id] = pcm_it->second.path;
                LOG_PERF("cache", "ns=pcm event=hit key={} module={}", pcm_key, module_name);
                co_return true;
            }
        }
        LOG_PERF("cache",
                 "ns=pcm event=miss reason={} key={} module={}",
                 pcm_miss,
                 pcm_key,
                 module_name);

        // Same shared-artifact budget as the PCH, but keyed with the
        // module's current content: unlike pch_key (which embeds the
        // preamble text), pcm_key is content-free, and a blocked budget
        // must unlock the moment the poison is edited.
        auto content = llvm::MemoryBuffer::getFile(file_path);
        auto budget_key = std::format("{}-{:016x}",
                                      pcm_key,
                                      content ? llvm::xxh3_64bits((*content)->getBuffer()) : 0);
        if(workspace.build_crashes.blocked(budget_key)) {
            LOG_WARN("PCM build for module {} refused: key {} keeps crashing workers",
                     module_name,
                     budget_key);
            co_return false;
        }

        bp.module_name = module_name;
        auto pending = workspace.store->begin_store("pcm", pcm_key);
        bp.output_path = pending.tmp_path;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        // Exclude the module being built — its old PCM path may still be
        // in pcm_paths from a previous (now-invalidated) build.
        workspace.fill_pcm_deps(bp.pcms, path_id);

        auto result = co_await send_stateless_retrying(
            pool,
            bp,
            [this, &budget_key](const kota::ipc::protocol::Error&) {
                workspace.build_crashes.on_crash(budget_key);
            });
        if(!result.has_value() || !result.value().success) {
            workspace.store->abort(pending);
            if(expected_build_failure(result)) {
                LOG_WARN("BuildPCM failed for module {}: {}",
                         module_name,
                         build_failure_message(result));
            } else {
                LOG_ANOMALY(PCMBuildFail,
                            "PCM build failed for module {}: {}",
                            module_name,
                            build_failure_message(result));
            }
            co_return false;
        }

        // Commit on the thread pool: it fsyncs the freshly written PCM.
        auto committed =
            co_await kota::queue([&] { return workspace.store->commit(std::move(pending)); });
        if(!committed.has_value() || !committed.value().has_value()) {
            LOG_WARN("Failed to commit PCM for module {}", module_name);
            co_return false;
        }

        workspace.build_crashes.on_land(budget_key);
        auto pcm_path = std::move(committed.value().value());
        workspace.pcm_paths[path_id] = pcm_path;
        workspace.pcm_cache[path_id] = {pcm_path,
                                        pcm_key,
                                        capture_deps_snapshot(workspace.path_pool,
                                                              result.value().deps,
                                                              result.value().build_at)};
        LOG_INFO("Built PCM for module {}: {}", module_name, pcm_path);

        // Persist cache metadata after successful build.
        workspace.save_cache(contexts);

        // Signal that new index data is available for background merge.
        if(on_indexing_needed)
            on_indexing_needed();

        co_return true;
    };

    workspace.compile_graph =
        std::make_unique<CompileGraph>(loop, std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", workspace.path_to_module.size());
}

std::string uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
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

kota::task<bool> Compiler::ensure_pch(Session& session,
                                      std::uint64_t launch_generation,
                                      std::uint64_t launch_epoch,
                                      const std::string& directory,
                                      const std::vector<std::string>& arguments) {
    // A round invalidated during the caller's earlier awaits (module
    // dependencies) must not touch pch_key at all: the reset and cache-hit
    // branches below write it before the first suspension point.
    if(!may_write_pch_key(session, launch_generation, launch_epoch)) {
        co_return false;
    }

    auto path_id = session.path_id;
    auto path = workspace.path_pool.resolve(path_id);
    auto& text = session.text;
    auto bound = compute_preamble_bound(text);
    auto* header_context = contexts.header_context(path_id);
    bool has_prefix = header_context && !header_context->preamble_path.empty();
    if(bound == 0 && !has_prefix) {
        // No preamble directives and no injected -include — PCH would be
        // empty. Self-contained header contexts land here too: they borrow
        // a command but inject nothing.
        session.pch_key.reset();
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

    // Reuse an existing PCH with the same content key — possibly built for
    // a different file.  The store lookup refreshes the blob's LRU position
    // and catches eviction.
    llvm::StringRef pch_miss = "no_entry";
    if(auto it = workspace.pch_cache.find(pch_key); it != workspace.pch_cache.end()) {
        auto& st = it->second;
        // Both halves of the pair must be present: a PCH whose
        // PreambleState blob is gone (crash between commits, failed aux
        // commit) rebuilds whole.
        bool in_store = workspace.store && workspace.store->lookup("pch", pch_key) &&
                        workspace.store->lookup_aux("pch", pch_key);
        if(st.path.empty()) {
            pch_miss = "incomplete_entry";
        } else if(!in_store) {
            pch_miss = "evicted";
        } else if(st.index_path.empty()) {
            // load_state() found the blob unreadable earlier; republish the
            // pair rather than serving a PCH with no index forever.
            pch_miss = "idx_unreadable";
        } else if(deps_changed(workspace.path_pool, st.deps)) {
            pch_miss = "deps_changed";
        } else {
            session.pch_key = pch_key;
            // Adopting a proven-good artifact disproves the session's PCH
            // strikes as surely as building one.
            session.quarantine.on_kind_land(evidence_kind(worker::BuildKind::BuildPCH));
            LOG_PERF("cache", "ns=pch event=hit key={} file={}", pch_key, path);
            co_return true;
        }
        // Blob evicted by the store's LRU: drop the metadata too, or the
        // content-keyed map grows for the server's lifetime.
        if(!in_store && !st.building) {
            workspace.pch_cache.erase(it);
        }
    }
    LOG_PERF("cache", "ns=pch event=miss reason={} key={} file={}", pch_miss, pch_key, path);

    // Preamble incomplete (user still typing) — defer rebuild, keep using
    // the session's previous PCH if it is still available.
    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        if(session.pch_key.has_value()) {
            auto it = workspace.pch_cache.find(*session.pch_key);
            co_return it != workspace.pch_cache.end() && !it->second.path.empty();
        }
        co_return false;
    }

    // If another coroutine is already building a PCH with this key
    // (same file, or another file with an identical preamble), wait for it.
    if(auto it = workspace.pch_cache.find(pch_key);
       it != workspace.pch_cache.end() && it->second.building) {
        co_await it->second.building->wait();
        // Guard the pch_key write below against an invalidated round's
        // continuation: a newer round (or a context switch) may have
        // established the session's PCH identity while we waited.
        if(!may_write_pch_key(session, launch_generation, launch_epoch)) {
            co_return false;
        }
        if(auto it2 = workspace.pch_cache.find(pch_key);
           it2 != workspace.pch_cache.end() && !it2->second.path.empty()) {
            session.pch_key = pch_key;
            session.quarantine.on_kind_land(evidence_kind(worker::BuildKind::BuildPCH));
            co_return true;
        }
        co_return false;
    }

    // A preamble whose PCH build keeps killing workers is refused before
    // the dispatch: the artifact is shared, so one document's quarantine
    // cannot contain it — every session with this preamble would burn
    // workers of its own. The key is content-derived: editing the poison
    // starts a fresh key with a fresh budget.
    if(workspace.build_crashes.blocked(pch_key)) {
        LOG_WARN("PCH build for {} refused: key {} keeps crashing workers", path, pch_key);
        co_return false;
    }

    // Register in-flight build so concurrent requests wait on us.  The
    // guard wakes them on every exit, including cancellation mid-await.
    auto completion = std::make_shared<kota::event>();
    workspace.pch_cache[pch_key].building = completion;
    BuildingGuard guard{workspace, pch_key, completion};

    if(!workspace.store) {
        LOG_WARN("PCH build skipped: cache store is unavailable");
        co_return false;
    }

    // Build a new PCH pair via stateless worker: it writes the PCH and its
    // PreambleState blob to the tmp paths allocated here; the store
    // commits (fsync + rename) both on success, primary first.
    auto pending = workspace.store->begin_store("pch", pch_key);
    auto pending_idx = workspace.store->begin_store_aux("pch", pch_key);

    worker::BuildParams bp;
    bp.priority = worker::Priority::High;
    bp.kind = worker::BuildKind::BuildPCH;
    bp.file = std::string(path);
    bp.directory = directory;
    bp.arguments = arguments;
    bp.text = text;
    bp.preamble_bound = bound;
    bp.output_path = pending.tmp_path;
    bp.index_output_path = pending_idx.tmp_path;

    LOG_DEBUG("Building PCH for {}, bound={}, key={}", path, bound, pch_key);

    // Each worker kill lands in two ledgers by design: the session's (the
    // preamble is this document's content) and the shared key's (other
    // sessions with the same preamble must stop re-triggering the build).
    auto result = co_await send_stateless_retrying(
        pool,
        bp,
        [this, &session, &pch_key](const kota::ipc::protocol::Error& error) {
            session.quarantine.on_kind_crash(evidence_kind(worker::BuildKind::BuildPCH),
                                             worker::death_of(error));
            workspace.build_crashes.on_crash(pch_key);
        });

    if(!result.has_value() || !result.value().success) {
        workspace.store->abort(pending);
        workspace.store->abort(pending_idx);
        if(expected_build_failure(result)) {
            LOG_WARN("PCH build failed for {}: {}", path, build_failure_message(result));
        } else {
            LOG_ANOMALY(PCHBuildFail,
                        "PCH build failed for {}: {}",
                        path,
                        build_failure_message(result));
        }
        co_return false;
    }

    // Commit the pair on the thread pool as one job: the fsyncs stay off
    // the event loop, and no cancellation can land between the two commits
    // — the store either publishes the whole pair or retracts it (a half
    // pair would let waiters adopt a PCH whose blob is gone). Opening the
    // freshly committed blob (mmap + flatbuffer verification, which walks
    // the whole file) also happens here so no later consumer pays that
    // walk on the event loop.
    struct PairCommit {
        std::optional<std::string> pch_path;
        std::optional<std::string> index_path;
        std::shared_ptr<index::PreambleState> state;
    };

    auto committed = co_await kota::queue([&]() -> PairCommit {
        PairCommit outcome;
        auto pch_path = workspace.store->commit(std::move(pending));
        if(!pch_path) {
            workspace.store->abort(pending_idx);
            return outcome;
        }
        outcome.pch_path = std::move(*pch_path);

        // The pair is only usable complete: when the index blob cannot be
        // published, retract the PCH too — the next compile rebuilds both.
        auto index_path = workspace.store->commit(std::move(pending_idx));
        if(!index_path) {
            workspace.store->invalidate("pch", pch_key);
            return outcome;
        }
        outcome.index_path = std::move(*index_path);
        outcome.state = index::PreambleState::load(*outcome.index_path);
        return outcome;
    });
    if(!committed.has_value() || !committed.value().pch_path.has_value()) {
        LOG_WARN("Failed to commit PCH for {}", path);
        co_return false;
    }
    if(!committed.value().index_path.has_value()) {
        LOG_WARN("Failed to commit PreambleState blob for {}", path);
        // A rebuild of an existing key just had its blobs retracted from
        // the store; the entry's paths now dangle and waiters checking
        // `!path.empty()` would hand the compile a deleted PCH. Drop it —
        // the guard tolerates a missing entry.
        workspace.pch_cache.erase(pch_key);
        co_return false;
    }

    // The key built: its strikes were transient, not poison. The session
    // ledger clears only when this build's launch is still current — a
    // stale build must not launder strikes the new content recorded.
    workspace.build_crashes.on_land(pch_key);
    if(session.generation == launch_generation) {
        session.quarantine.on_kind_land(evidence_kind(worker::BuildKind::BuildPCH));
    }

    auto& st = workspace.pch_cache[pch_key];
    st.path = *committed.value().pch_path;
    st.bound = bound;
    st.deps =
        capture_deps_snapshot(workspace.path_pool, result.value().deps, result.value().build_at);
    st.index_path = *committed.value().index_path;
    // Replace the previous blob's mapping (same key, rebuilt content);
    // in-flight holders of the old shared_ptr stay valid.
    st.state = committed.value().state;

    LOG_INFO("PCH built for {}: {}", path, st.path);

    // Persist cache metadata after successful build.
    workspace.save_cache(contexts);

    // The cache entry above is content-keyed and correct regardless; only
    // the session pointer must not be written by an invalidated round.
    if(!may_write_pch_key(session, launch_generation, launch_epoch)) {
        co_return false;
    }
    session.pch_key = pch_key;

    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
kota::task<bool> Compiler::ensure_deps(Session& session,
                                       std::uint64_t launch_generation,
                                       std::uint64_t launch_epoch,
                                       const std::string& directory,
                                       const std::vector<std::string>& arguments,
                                       std::pair<std::string, uint32_t>& pch,
                                       std::unordered_map<std::string, std::string>& pcms,
                                       std::optional<kota::cancellation_token> scope) {
    auto path_id = session.path_id;

    // Compile module dependencies within the request scope: cancelling the
    // scope unwinds the wait and releases this request's interest in the
    // dependency graph, without touching the shared compilations themselves.
    auto compile_deps = [&](std::uint32_t pid) -> kota::task<bool> {
        if(!scope) {
            co_return co_await workspace.compile_graph->compile_deps(pid);
        }
        auto result = co_await kota::with_token(workspace.compile_graph->compile_deps(pid), *scope);
        co_return result.has_value() && *result;
    };

    // Re-validate cached PCM blobs and compile module dependencies.  LRU
    // eviction can remove a blob while its compile unit is still marked
    // clean, so dirty those units instead of handing clang a dangling
    // path.  Building dependencies can itself evict another clean
    // module's PCM under budget pressure, which reopens the window the
    // scan just closed — hence the bounded retry until the set is stable.
    //
    // FIXME: this scans every pcm_paths entry (one stat() per module) on
    // every compile, even in steady state when nothing was evicted.  For
    // large modular projects on NFS this adds measurable latency.  Consider
    // having CacheStore notify on eviction or caching the scan result.
    if(workspace.compile_graph) {
        for(int attempt = 0; attempt < 3; ++attempt) {
            llvm::SmallVector<std::uint32_t> evicted;
            for(auto& [pid, pcm_path]: workspace.pcm_paths) {
                if(!llvm::sys::fs::exists(pcm_path)) {
                    evicted.push_back(pid);
                }
            }
            if(attempt > 0 && evicted.empty()) {
                break;
            }

            for(auto pid: evicted) {
                for(auto id: workspace.compile_graph->update(pid)) {
                    workspace.pcm_paths.erase(id);
                    workspace.pcm_cache.erase(id);
                }
                workspace.pcm_paths.erase(pid);
                workspace.pcm_cache.erase(pid);
            }

            if(!co_await compile_deps(path_id)) {
                co_return false;
            }
        }
    }

    // Scan buffer text for module imports that might not be in compile_graph yet.
    // When a user adds `import std;` without saving, the compile_graph (disk-based)
    // doesn't know about the new dependency. Scan the in-memory text to find them.
    {
        auto scan_result = scan(session.text);
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
                if(workspace.compile_graph && workspace.compile_graph->has_unit(module_pid)) {
                    co_await compile_deps(module_pid);
                }
            }
        }
    }

    // The buffer-scan waits above tolerate failed PCM builds, but a cancelled
    // scope means this round was superseded — abandon it before the PCH step.
    if(scope && scope->cancelled()) {
        co_return false;
    }

    // Build or reuse PCH.
    auto pch_ok =
        co_await ensure_pch(session, launch_generation, launch_epoch, directory, arguments);
    if(pch_ok && session.pch_key.has_value()) {
        if(auto pch_it = workspace.pch_cache.find(*session.pch_key);
           pch_it != workspace.pch_cache.end()) {
            pch = {pch_it->second.path, pch_it->second.bound};
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
/// Called lazily by forward_query() / forward_build() before every
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

    auto finish_compile = [&]() {
        if(session->compiling == pc) {
            session->compiling.reset();
        }
        LOG_INFO("ensure_compiled: finish path_id={}", pid);
        pc->done.set();
    };

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
    for(int attempt = 0; attempt < 2; ++attempt) {
        worker::CompileParams params;
        params.path = file_path;
        params.version = session->version;
        params.text = session->text;
        auto source =
            contexts.resolve_command(file_path, params.directory, params.arguments, session.get());

        // The line the appended suffix #include lands on — anything at or
        // past it is phantom text the user cannot see.
        std::optional<std::uint32_t> suffix_line_limit;
        auto* header_context = contexts.header_context(session->path_id);
        if(header_context && !header_context->suffix_path.empty()) {
            auto newlines = std::ranges::count(params.text, '\n');
            suffix_line_limit =
                static_cast<std::uint32_t>(newlines + (params.text.ends_with('\n') ? 0 : 1));
        }
        contexts.append_suffix_include(*session, params.text);

        // Whether this round is the self-containment probe: a header
        // deliberately compiled without its includer prefix to see if it
        // stands alone. Decided here, where resolve_command chose to omit
        // the prefix; the landing gates what the probe may write.
        bool trial_round = attempt == 0 && !session->trial_done && header_context &&
                           header_context->preamble_path.empty() &&
                           contexts.header_mode(file_path, pid) == HeaderMode::Unknown;

        bool deps_ok = co_await ensure_deps(*session,
                                            gen,
                                            epoch,
                                            params.directory,
                                            params.arguments,
                                            params.pch,
                                            params.pcms,
                                            pc->deps_scope.token());
        pc->deps_done = true;
        if(!deps_ok) {
            LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
            finish_compile();
            co_return;
        }

        if(session->generation != gen) {
            LOG_INFO("ensure_compiled: superseded before send ({} vs {}) for {}",
                     session->generation,
                     gen,
                     uri_str);
            finish_compile();
            co_return;
        }

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
            finish_compile();
            co_return;
        }

        // Seed the inactive-region scan with the conditional stack the
        // PCH's preamble left open (a #if cut by the bound). Copy the state
        // out: concurrent compiles can insert into pch_cache across the
        // await below and rehash the map from under a held pointer.
        std::vector<std::uint32_t> pch_inactive;
        std::shared_ptr<index::PreambleState> preamble_state;
        if(session->pch_key.has_value()) {
            auto it = workspace.pch_cache.find(*session->pch_key);
            preamble_state = it != workspace.pch_cache.end() ? it->second.load_state() : nullptr;
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
            finish_compile();
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
            finish_compile();
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
            finish_compile();
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
                    kota::codec::json::from_json(result.value().diagnostics.data, diagnostics);
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

        if(!result.value().tu_index_data.empty()) {
            auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
            session->file_index = std::move(tu_index.main_file_index);
            session->symbols = std::move(tu_index.symbols);
        } else {
            // The AST and the file index settle together — that pairing is
            // what lets navigation trust the index after ensure_compiled. A
            // compile that produced no index data (fatal error, no AST) must
            // therefore drop the previous buffer's index rather than leave
            // it posing as current: an honest gap over yesterday's offsets.
            session->file_index.reset();
            session->symbols.reset();
        }

        auto version = session->version;
        finish_compile();

        LOG_PERF("request", "kind=Compile file={} total_ms={}", file_path, timer.ms());
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
        if(on_indexing_needed)
            on_indexing_needed();
        co_return;
    }

    finish_compile();
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
        if(pending->generation != session->generation && !pending->deps_done) {
            // The in-flight compile is stale (user edited since it started)
            // and still holds interest in the module graph — supersede it.
            // A stale compile already past its dependency phase is left to
            // finish instead: superseding it gains nothing (the worker send
            // is not cancellable), and waiting coalesces rapid edits into a
            // single follow-up compile at the latest generation.
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

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind,
                                            std::shared_ptr<Session> session,
                                            std::optional<protocol::Position> position,
                                            std::optional<protocol::Range> range) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;
    auto map = session->line_map();

    ScopedTimer timer;
    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }
    auto wait_ms = timer.ms();

    if(session->generation != gen) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp;
    wp.kind = kind;
    wp.path = path;

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
    auto result = co_await pool.send_stateful(path_id, wp, {}, suspect);
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
    LOG_PERF("request", "kind={} file={} wait_ms={} total_ms={}", kind, path, wait_ms, timer.ms());
    co_return std::move(result.value());
}

kota::task<std::vector<feature::DocumentLink>, kota::ipc::Error>
    Compiler::forward_document_links(std::shared_ptr<Session> session) {
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
    auto wait_ms = timer.ms();

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
    auto result =
        co_await pool.send_stateful(path_id, worker::DocumentLinkParams{path}, {}, suspect);
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
             "kind=DocumentLink file={} wait_ms={} total_ms={}",
             path,
             wait_ms,
             timer.ms());
    co_return std::move(result.value());
}

Compiler::RawResult Compiler::forward_build(worker::BuildKind kind,
                                            const protocol::Position& position,
                                            std::shared_ptr<Session> session) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // This build compiles the same content the quarantine watches: while
    // the document is quarantined, only the recovery dispatch — the kind
    // holding the strikes, with the probe armed — may run. Anything else
    // is arbitrary work on proven-poisonous content. A refusal announces
    // the quarantine, or a completion-only client would never see it.
    if(session->quarantine.active() && !session->quarantine.recovery_kind(evidence_kind(kind))) {
        LOG_WARN("forward_build: {} is quarantined, refusing build", path);
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

    worker::BuildParams wp;
    wp.priority = worker::Priority::High;
    wp.kind = kind;
    wp.file = path;
    wp.version = session->version;
    wp.text = session->text;
    contexts.resolve_command(path, wp.directory, wp.arguments, session.get());
    contexts.append_suffix_include(*session, wp.text);

    ScopedTimer timer;
    if(!co_await ensure_deps(*session, gen, epoch, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        LOG_WARN("forward_build: dependency preparation failed for {}", path);
        co_return kota::outcome_error(kota::ipc::Error{"Dependency preparation failed"});
    }
    // A PCH crash inside ensure_deps may have tipped the document into
    // quarantine after the entry gate: stop before dispatching the same
    // content again — that crash also spent any armed probe (it WAS the
    // attempt). A probe that predates this request's own evidence does not
    // excuse dispatching content that just proved poisonous.
    if(session->quarantine.active() && session->quarantine.grew(flight)) {
        session->quarantine.spend_probe();
        LOG_WARN("forward_build: {} quarantined during dependency prep", path);
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    auto wait_ms = timer.ms();

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
    bool recovery = session->quarantine.recovery_kind(evidence_kind(kind));
    if(session->quarantine.active() && !recovery) {
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }
    std::optional<Quarantine::ProbeGuard> probe_guard;
    if(recovery) {
        probe_guard.emplace(session->quarantine);
    }
    auto result = co_await build_for(pool, *session, evidence_kind(kind), wp);
    if(!result.has_value()) {
        if(!worker::is_operational_error(result.error())) {
            LOG_ANOMALY(WorkerRequestFail,
                        "build (kind={}) failed for {}: {}",
                        kind,
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
        session->quarantine.on_kind_land(evidence_kind(kind));
        if(was_active && !session->quarantine.active()) {
            publish_recovered(session);
        }
    }
    LOG_PERF("request", "kind={} file={} wait_ms={} total_ms={}", kind, path, wait_ms, timer.ms());
    co_return std::move(result.value().result_json);
}

Compiler::RawResult Compiler::forward_format(std::shared_ptr<Session> session,
                                             std::optional<protocol::Range> range) {
    auto path_id = session->path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));
    auto gen = session->generation;

    // Formatting runs no sema, but it is still this document's content on
    // a worker: while quarantined, only format-as-recovery may run, and a
    // refusal announces the quarantine.
    bool recovery = session->quarantine.recovery_kind(evidence_kind(worker::BuildKind::Format));
    if(session->quarantine.active() && !recovery) {
        LOG_WARN("forward_format: {} is quarantined, refusing format", path);
        if(session->quarantine.needs_announcement()) {
            publish_quarantined(session, std::nullopt, std::nullopt);
        }
        co_return kota::outcome_error(
            kota::ipc::Error{worker::dispatch_errc::worker_unavailable, "Document is quarantined"});
    }

    worker::BuildParams wp;
    wp.priority = worker::Priority::High;
    wp.kind = worker::BuildKind::Format;
    wp.file = path;
    wp.text = session->text;

    if(range) {
        lsp::LineMap map(wp.text);
        wp.format_range = {clamped_offset(map, range->start), clamped_offset(map, range->end)};
        if(wp.format_range.begin > wp.format_range.end) {
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
    auto result = co_await build_for(pool, *session, evidence_kind(worker::BuildKind::Format), wp);
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
        session->quarantine.on_kind_land(evidence_kind(worker::BuildKind::Format));
        if(was_active && !session->quarantine.active()) {
            publish_recovered(session);
        }
    }
    LOG_PERF("request", "kind=Format file={} total_ms={}", path, timer.ms());
    co_return std::move(result.value().result_json);
}

}  // namespace clice
