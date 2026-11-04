module;

#include "kotatsu/ipc/framing.h"

module clice;

import :compile.compilation;
import :feature.code_actions;
import :feature.diagnostics;
import :feature.document_links;
import :feature.document_symbols;
import :feature.folding_ranges;
import :feature.hover;
import :feature.inactive_regions;
import :feature.inlay_hints;
import :feature.selection_ranges;
import :feature.semantic_tokens;
import :index.indexer;
import :index.tu_index;
import :support.json;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :worker.common;
import :worker.crash_report;
import :worker.gate;
import :worker.protocol;
import :worker.stateful;

namespace clice {

using ipc::RequestResult;
using RequestContext = ipc::BincodePeer::RequestContext;

/// A compile past its parse, while clang instantiates the templates the file
/// uses (CompilationParams::Phases), and the requests reading its AST
/// meanwhile: a request holds the compile where it pauses, where nothing
/// changes the AST, and lets it go on once it read it.
struct CompileRun : std::enable_shared_from_this<CompileRun> {
    CompileRun(int version, async::event_loop& loop) :
        version(version), relay(loop.create_relay()) {}

    enum class State : std::uint8_t {
        /// The compile may change the AST.
        Running,
        /// The compile waits where it paused, for a request reading the AST.
        Paused,
        /// The compile no longer reads the AST.
        Done,
    };

    /// On the compile's thread: wait while a request reads the AST.
    void pause() {
        std::unique_lock lock(mutex);
        if(!held) {
            return;
        }
        state = State::Paused;
        signal();
        resumed.wait(lock, [&] { return state == State::Running; });
    }

    /// On the compile's thread, as the instantiations begin: `begin` runs
    /// paused, where a request may read the AST, and the compile goes on
    /// once none does.
    void begin_instantiations(llvm::function_ref<void()> begin) {
        {
            std::lock_guard lock(mutex);
            state = State::Paused;
            beginning = true;
            signal();
        }
        begin();
        std::unique_lock lock(mutex);
        resumed.wait(lock, [&] { return !held; });
        beginning = false;
        state = State::Running;
    }

    /// On the compile's thread, its last touch of the AST.
    void done() {
        std::lock_guard lock(mutex);
        state = State::Done;
        signal();
    }

    /// Hold the compile where nothing changes the AST.
    async::task<> hold() {
        while(true) {
            {
                std::lock_guard lock(mutex);
                held = true;
                if(state != State::Running) {
                    co_return;
                }
            }
            co_await changed.wait();
            changed.reset();
        }
    }

    /// Let the compile go on.
    void release() {
        std::lock_guard lock(mutex);
        held = false;
        if(state == State::Paused) {
            if(!beginning) {
                state = State::Running;
            }
            resumed.notify_one();
        }
    }

    /// From the compile's thread: the state left Running.
    void signal() {
        relay.send([self = weak_from_this()] {
            if(auto run = self.lock()) {
                run->changed.set();
            }
        });
    }

    int version;

    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);

    /// The Compile reply: the parse's, else the whole compile's.
    worker::CompileResult parsed_result;

    async::event parsed{false};

    async::event finished{false};

    /// Set on the loop as the state leaves Running.
    async::event changed{false};

    /// From the compile's thread to the loop; keeps the loop alive.
    async::relay relay;

    /// Orders the state between the compile's thread and the loop.
    std::mutex mutex;

    std::condition_variable resumed;

    State state = State::Running;

    /// A request reads the AST or waits to.
    bool held = false;

    /// Paused where the instantiations begin, until the compile goes on by
    /// itself (begin_instantiations).
    bool beginning = false;

    /// The AST, from the end of the parse on.
    CompilationUnit* unit = nullptr;
};

struct DocumentEntry {
    /// The AST of the last compile that completed; null while a compile
    /// runs, and after one that did not complete.
    std::unique_ptr<CompilationUnit> unit;

    /// The compile running past its parse, whose AST answers meanwhile.
    std::shared_ptr<CompileRun> running;

    /// The reply of the last compile that ended past its parse, until a
    /// FinishCompile takes it or the next compile starts.
    std::optional<worker::CompileResult> finished;

    /// The mapping of the PCH the AST loads, held while the document stays
    /// here: its next compile reads the same one (vfs::keep_mapped).
    std::shared_ptr<const llvm::MemoryBuffer> pch_mapping;

    // Signaled when the first compilation finishes. Feature handlers
    // co_await this before accessing the AST.
    async::event ast_ready{false};

    /// The bound of the PCH the AST loads.
    std::uint32_t pch_bound = 0;

    /// The conditional stack the PCH's preamble left open.
    std::vector<std::uint8_t> open_conditionals;

    /// The inactive regions of the PCH's preamble.
    std::vector<std::uint32_t> preamble_inactive_regions;

    /// The AST requests read: the running compile's, else the last one's.
    CompilationUnit* ast() {
        return running ? running->unit : unit.get();
    }

    /// The preamble share from the compile params, then the own scan past
    /// the PCH bound, seeded by the conditional stack the preamble left
    /// open.
    std::vector<std::uint32_t> inactive_regions() {
        auto regions = preamble_inactive_regions;
        auto scan = feature::inactive_regions(*ast(), open_conditionals, pch_bound);
        regions.insert(regions.end(), scan.regions.begin(), scan.regions.end());
        return regions;
    }

    // Per-document serialization mutex
    async::mutex strand;
};

/// A request's hold on a document entry: keeps it alive through an Evict.
/// Taken as the request is dispatched, then moved into the task that
/// serves it; a moved-from hold holds nothing.
struct [[nodiscard]] EntryHold {
    std::shared_ptr<DocumentEntry> doc;

    explicit EntryHold(std::shared_ptr<DocumentEntry> entry) : doc(std::move(entry)) {}

    EntryHold(EntryHold&&) noexcept = default;
    EntryHold& operator=(EntryHold&&) = delete;

    ~EntryHold() {
        // The last hold on an evicted entry frees its AST: return the memory
        // as the eviction would have.
        if(doc && doc.use_count() == 1) {
            doc.reset();
            release_free_memory();
        }
    }
};

/// A compile's hold on its document, which wakes the AST waiters when
/// released: at every exit of the compile, and when a cancel read with the
/// request destroys its task before it starts. An unset ast_ready would
/// hang every later query for the document (they observe no unit and
/// return their missing value; the next Compile sets a real AST).
struct [[nodiscard]] CompileGuard {
    EntryHold hold;

    explicit CompileGuard(std::shared_ptr<DocumentEntry> entry) : hold(std::move(entry)) {}

    CompileGuard(CompileGuard&&) noexcept = default;
    CompileGuard& operator=(CompileGuard&&) = delete;

    ~CompileGuard() {
        if(hold.doc) {
            hold.doc->ast_ready.set();
        }
    }
};

/// The worker keeps the documents the master's pool sends it, until the
/// pool evicts them (WorkerPool, worker::EvictParams).
class StatefulWorker {
    ipc::BincodePeer& peer;

    llvm::StringMap<std::shared_ptr<DocumentEntry>> documents;

    std::shared_ptr<DocumentEntry> get_or_create(llvm::StringRef path) {
        auto [it, inserted] = documents.try_emplace(path, nullptr);
        if(inserted) {
            it->second = std::make_shared<DocumentEntry>();
            LOG_DEBUG("Created new document entry: {}", path.str());
        }
        return it->second;
    }

    /// Look up the document as the request is dispatched, then wait for its
    /// AST, lock its strand and run fn(doc) on the thread pool. Answers
    /// `missing` if the AST is unusable. `kind` discriminates the perf
    /// series: query kinds have very different costs and must not collapse
    /// into one distribution.
    template <typename Params, typename R, typename F>
    async::task<R, ipc::Error>
        with_ast_or(llvm::StringRef kind, const Params& params, R missing, F fn) {
        auto it = documents.find(params.path);
        assert(it != documents.end() && "the pool queries only documents it holds resident here");
        return serve_ast(kind, EntryHold(it->second), params, std::move(missing), std::move(fn));
    }

    template <typename Params, typename R, typename F>
    async::task<R, ipc::Error>
        serve_ast(llvm::StringRef kind, EntryHold hold, const Params& params, R missing, F fn) {
        auto& doc = *hold.doc;
        ScopedTimer timer;
        co_await doc.ast_ready.wait();
        auto strand = co_await doc.strand.scoped_lock();
        // Let go however the wait for the compile's pause ends: the compile
        // would wait for this request at its next pause otherwise.
        auto run = doc.running;
        auto release = llvm::make_scope_exit([&] {
            if(run) {
                run->release();
            }
        });
        if(run) {
            co_await run->hold();
        }
        auto acquire_ms = timer.ms_f();
        double compute_ms = 0;

        // The frame stays alive until fn returns even when the handler is
        // cancelled mid-await, so the by-reference captures are safe; the
        // hook just lets a cancelled query return its missing value instead
        // of walking the whole AST for a result nobody will read.
        std::atomic<bool> cancelled{false};
        auto result = co_await async::queue(
            [&]() -> R {
                if(cancelled.load(std::memory_order_relaxed)) {
                    return std::move(missing);
                }
                if(!doc.ast())
                    return std::move(missing);
                CrashScope crash_scope(worker::crash_tag(params));
                ScopedTimer compute_timer;
                auto value = fn(doc);
                compute_ms = compute_timer.ms_f();
                return value;
            },
            [&] { cancelled.store(true, std::memory_order_relaxed); });

        LOG_PERF("query",
                 "kind={} path={} acquire_ms={:.2f} compute_ms={:.2f} total_ms={:.2f}",
                 kind,
                 params.path,
                 acquire_ms,
                 compute_ms,
                 timer.ms_f());
        co_return result;
    }

    RequestResult<worker::CompileParams> serve_compile(CompileGuard guard,
                                                       const worker::CompileParams& params);

    /// The compile itself, running on past the Compile reply while clang
    /// instantiates at the end of the file.
    async::task<> run_compile(std::shared_ptr<DocumentEntry> doc,
                              std::shared_ptr<CompileRun> run,
                              worker::CompileParams params);

    RequestResult<worker::FinishCompileParams>
        serve_finish(EntryHold hold, const worker::FinishCompileParams& params);

public:
    StatefulWorker(ipc::BincodePeer& peer, async::event_loop& loop) : peer(peer), loop(loop) {}

    void register_handlers();

private:
    async::event_loop& loop;

    async::task_group<> compiles;
};

/// What a compile replies: the parse's AST, its diagnostics and its index,
/// or why there is none.
static worker::CompileResult result_of(CompilationUnit& unit,
                                       const worker::CompileParams& params,
                                       llvm::StringRef index_output_path,
                                       bool stopped) {
    worker::CompileResult result;
    result.version = params.version;

    // A failed parse whose diagnostics point at the consumed PCH: either a
    // diagnostic names the blob's path outright, or it is an
    // AST-deserialization error naming no other prebuilt input
    // (that family's messages do not reliably carry the path —
    // "malformed or corrupted precompiled file: 'Blob ends too
    // soon'"). The reader rejects a blob at setup, or finds it
    // corrupt halfway through, which is fatal. User-code failures
    // (missing include, modified header, bad flags) match neither,
    // so the master never rebuilds an innocent shared PCH over a
    // failure it did not cause. An anonymous read error with PCMs
    // in play is ambiguous; suspecting the PCH costs at most one
    // retracted pair per attempt and self-corrects on the retry.
    if((!unit.completed() || unit.fatal_error()) && !params.pch.first.empty()) {
        auto points_at_pch = [&](const Diagnostic& diag) {
            llvm::StringRef message = diag.message;
            if(message.contains(params.pch.first)) {
                return true;
            }
            if(!diag.id.is_deserialization_error()) {
                return false;
            }
            return ranges::none_of(params.pcms,
                                   [&](auto& entry) { return message.contains(entry.second); });
        };
        result.pch_suspect = ranges::any_of(unit.diagnostics(), [&](auto& diag) {
            return points_at_pch(diag) || ranges::any_of(diag.notes, points_at_pch);
        });
    }

    if(!unit.completed() || stopped) {
        result.status =
            unit.setup_fail() ? worker::CompileStatus::SetupFail : worker::CompileStatus::Cancelled;
        if(unit.setup_fail()) {
            result.diagnostics = feature::diagnostics(unit);
        }
        return result;
    }

    result.started_at = unit.started_at().count();
    result.deps = unit.deps();
    auto index_error =
        send_index(index::build_tu_index(
                       unit,
                       {.known_instantiations = params.known_instantiations,
                        .standard_library_instantiations = params.standard_library_instantiations}),
                   index_output_path,
                   result);
    result.diagnostics = feature::diagnostics(unit);
    if(index_error) {
        result.diagnostics.push_back(feature::index_unavailable(*index_error));
    }
    return result;
}

RequestResult<worker::CompileParams>
    StatefulWorker::serve_compile(CompileGuard guard, const worker::CompileParams& params) {
    auto doc = guard.hold.doc;
    // A compile still instantiating describes the text this one replaces.
    if(auto previous = doc->running) {
        previous->stop->store(true, std::memory_order_relaxed);
        co_await previous->finished.wait();
    }
    auto strand = co_await doc->strand.scoped_lock();

    // The old AST describes the text this request just replaced: drop
    // it before the cancellable await, or a cancellation arriving while
    // the work is still queued would wake waiters with the previous
    // unit installed next to the new buffer — with_ast_or would serve
    // stale offsets as current. Waiters observe no unit and return their
    // missing value until a compile finishes.
    doc->unit.reset();
    doc->finished.reset();

    auto run = std::make_shared<CompileRun>(params.version, loop);
    doc->running = run;
    if(!compiles.spawn(run_compile(doc, run, params))) {
        doc->running.reset();
        co_await async::fail(ipc::Error{worker::dispatch_errc::cancelled, "Worker stopping"});
    }
    // A Compile the master interrupts before the parse ended stops it.
    auto interrupted = llvm::make_scope_exit([&] {
        if(!run->parsed.is_set()) {
            run->stop->store(true, std::memory_order_relaxed);
        }
    });
    co_await run->parsed.wait();
    co_return std::move(run->parsed_result);
}

async::task<> StatefulWorker::run_compile(std::shared_ptr<DocumentEntry> doc,
                                          std::shared_ptr<CompileRun> run,
                                          worker::CompileParams params) {
    std::weak_ptr<CompileRun> self = run;
    std::unique_ptr<CompilationUnit> unit;
    std::shared_ptr<const llvm::MemoryBuffer> pch_mapping;
    // The parse is interruptible: CompilationParams::stop is polled after
    // every top-level declaration and between the instantiations past it,
    // so a newer compile reaches into the middle of this one instead of
    // waiting for it to finish.
    auto result = co_await async::queue(
        [&]() -> worker::CompileResult {
            CrashScope crash_scope(worker::crash_tag(params));
            // Past the parse a crash is the FinishCompile's, the request
            // then in flight.
            std::optional<CrashScope> finishing;
            ScopedTimer timer;

            CompilationParams cp{
                .kind = CompilationKind::Content,
                .input = {.arguments = params.arguments,
                          .directory = params.directory,
                          .project_root = params.project_root,
                          .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
                .tidy = params.tidy,
                .preamble_inactive_regions = params.preamble_inactive_regions,
                .stop = run->stop,
                .phases = {
                          .parsed =
                        [&](CompilationUnit& parsed) {
                            auto result =
                                result_of(parsed, params, params.index_output_path, false);
                            result.unfinished = true;
                            LOG_INFO("Compile parsed: path={}, {}ms, {} diags",
                                     params.path,
                                     timer.ms(),
                                     result.diagnostics.size());
                            {
                                std::lock_guard lock(run->mutex);
                                run->unit = &parsed;
                            }
                            run->relay.send([self, result = std::move(result)]() mutable {
                                if(auto run = self.lock()) {
                                    run->parsed_result = std::move(result);
                                    run->parsed.set();
                                }
                            });
                            // A test gate parks the instantiations
                            // here, while the AST answers.
                            run->begin_instantiations([&] {
                                finishing.emplace(worker::crash_tag(
                                    worker::FinishCompileParams{.path = params.path}));
                            });
                        }, .pause = [&] { run->pause(); },
                          },
            };
            pch_mapping = use_precompiled(cp.input, params.pch, params.pcms);
            cp.input.add_focus(params.path, params.text);
            cp.input.add_synthesized(params.synthesized);

            unit = compile(std::move(cp));
            // The parse's reply took the transfer file the master gave it.
            auto result = result_of(*unit,
                                    params,
                                    unit->parsed() ? params.finished_index_output_path
                                                   : params.index_output_path,
                                    run->stop->load(std::memory_order_relaxed));
            LOG_INFO("Compile done: path={}, {}ms, {} diags, status={}",
                     params.path,
                     timer.ms(),
                     result.diagnostics.size(),
                     static_cast<int>(result.status));
            run->done();
            return result;
        },
        [stop = run->stop] { stop->store(true, std::memory_order_relaxed); });

    // Without a parse that ended, the Compile still waits: this is its
    // reply, and it holds the strand until it has it.
    bool parsed = unit->parsed();
    auto status = result.status;
    if(parsed) {
        co_await run->parsed.wait();
    } else {
        run->parsed_result = std::move(result);
        run->parsed.set();
    }
    auto strand = co_await doc->strand.scoped_lock();
    if(status == worker::CompileStatus::Done) {
        doc->unit = std::move(unit);
        doc->pch_mapping = std::move(pch_mapping);
        doc->pch_bound = params.pch.second;
        doc->open_conditionals = params.open_conditionals;
        doc->preamble_inactive_regions = params.preamble_inactive_regions;
    } else {
        // An incomplete unit can never serve a query, yet it pins the
        // consumed PCHs and PCMs — on Windows a mapped PCH cannot be
        // replaced on disk, so holding it would block the master's
        // rebuild of a retracted pair.
        unit.reset();
        doc->pch_mapping.reset();
    }
    if(doc->running == run) {
        doc->running.reset();
    }
    if(parsed) {
        doc->finished = std::move(result);
    }
    run->finished.set();
    release_free_memory();
}

RequestResult<worker::FinishCompileParams>
    StatefulWorker::serve_finish(EntryHold hold, const worker::FinishCompileParams& params) {
    auto& doc = *hold.doc;
    if(auto run = doc.running; run && run->version == params.version) {
        co_await run->finished.wait();
    }
    if(doc.finished && doc.finished->version == params.version) {
        co_return *std::exchange(doc.finished, std::nullopt);
    }
    co_return worker::CompileResult{.status = worker::CompileStatus::Cancelled,
                                    .version = params.version};
}

void StatefulWorker::register_handlers() {
    // === Compile ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CompileParams& params) -> RequestResult<worker::CompileParams> {
            LOG_INFO("Compile request: path={}, version={}", params.path, params.version);
            CompileGuard guard(get_or_create(params.path));
            return serve_compile(std::move(guard), params);
        });

    // === DocumentLink ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentLinkParams& params)
                        -> RequestResult<worker::DocumentLinkParams> {
        return with_ast_or("DocumentLink",
                           params,
                           std::vector<index::DocumentLink>{},
                           [&](DocumentEntry& doc) { return feature::document_links(*doc.ast()); });
    });

    // === FoldingRange ===
    peer.on_request([this](RequestContext& ctx, const worker::FoldingRangeParams& params)
                        -> RequestResult<worker::FoldingRangeParams> {
        return with_ast_or(
            "FoldingRange",
            params,
            std::optional<std::vector<feature::FoldingRange>>{},
            [&](DocumentEntry& doc) { return std::optional(feature::folding_ranges(*doc.ast())); });
    });

    // === SemanticTokens ===
    peer.on_request([this](RequestContext& ctx, const worker::SemanticTokensParams& params)
                        -> RequestResult<worker::SemanticTokensParams> {
        return with_ast_or(
            "SemanticTokens",
            params,
            std::optional<std::vector<feature::SemanticToken>>{},
            [&](DocumentEntry& doc) {
                auto regions = doc.inactive_regions();
                return std::optional(
                    feature::semantic_tokens(*doc.ast(), regions, params.settings.semantic_tokens));
            });
    });

    // === DocumentSymbol ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentSymbolParams& params)
                        -> RequestResult<worker::DocumentSymbolParams> {
        return with_ast_or("DocumentSymbol",
                           params,
                           std::optional<std::vector<feature::DocumentSymbol>>{},
                           [&](DocumentEntry& doc) {
                               auto regions = doc.inactive_regions();
                               return std::optional(
                                   feature::document_symbols(*doc.ast(),
                                                             regions,
                                                             params.settings.document_symbols));
                           });
    });

    // === CodeAction ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CodeActionParams& params) -> RequestResult<worker::CodeActionParams> {
            return with_ast_or("CodeAction",
                               params,
                               std::vector<feature::CodeAction>{},
                               [&](DocumentEntry& doc) {
                                   return feature::code_actions(*doc.ast(), params.range);
                               });
        });

    // === SelectionRange ===
    peer.on_request([this](RequestContext& ctx, const worker::SelectionRangeParams& params)
                        -> RequestResult<worker::SelectionRangeParams> {
        return with_ast_or("SelectionRange",
                           params,
                           std::vector<std::vector<LocalSourceRange>>{},
                           [&](DocumentEntry& doc) {
                               return feature::selection_ranges(*doc.ast(), params.offsets);
                           });
    });

    // === InlayHint ===
    peer.on_request([this](RequestContext& ctx, const worker::InlayHintParams& params)
                        -> RequestResult<worker::InlayHintParams> {
        return with_ast_or(
            "InlayHint",
            params,
            std::vector<feature::InlayHint>{},
            [&](DocumentEntry& doc) {
                return feature::inlay_hints(*doc.ast(), params.range, params.settings.inlay_hints);
            });
    });

    // === FinishCompile ===
    peer.on_request([this](RequestContext& ctx, const worker::FinishCompileParams& params)
                        -> RequestResult<worker::FinishCompileParams> {
        auto it = documents.find(params.path);
        assert(it != documents.end() && "the pool queries only documents it holds resident here");
        return serve_finish(EntryHold(it->second), params);
    });

    // === Evict ===
    peer.on_notification([this](const worker::EvictParams& params) {
        LOG_DEBUG("Evict notification: path={}", params.path);
        if(auto it = documents.find(params.path); it != documents.end() && it->second->running) {
            it->second->running->stop->store(true, std::memory_order_relaxed);
        }
        documents.erase(params.path);
        release_free_memory();
    });

    // === Hover ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::HoverParams& params) -> RequestResult<worker::HoverParams> {
            return with_ast_or("Hover",
                               params,
                               std::optional<feature::HoverInfo>{},
                               [&](DocumentEntry& doc) {
                                   return feature::hover_info(*doc.ast(),
                                                              params.offset,
                                                              params.settings.hover,
                                                              params.settings.documentation);
                               });
        });
}

int run_stateful_worker_mode(const std::string& worker_name, const std::string& log_dir) {
    logging::stderr_logger(worker_name, logging::options);
    if(!log_dir.empty()) {
        // File only: worker stderr is reserved for crash/unexpected output,
        // which the master relays into its own log (see logging taxonomy).
        logging::file_logger(worker_name, log_dir, logging::options, /*mirror_stderr=*/false);
    }

    LOG_INFO("Starting stateful worker");
    install_crash_report();

    async::event_loop loop;

    auto transport_result = ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }
    (*transport_result)->set_remote_max_payload(ipc::default_max_payload);

    ipc::BincodePeer peer(loop, std::move(*transport_result));
    install_test_gates(peer, loop);

    StatefulWorker worker(peer, loop);
    worker.register_handlers();

    LOG_INFO("Stateful worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateful worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
