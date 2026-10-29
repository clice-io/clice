module;

#include "kota/ipc/framing.h"

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

struct DocumentEntry {
    int version = 0;
    std::string text;

    /// The AST of the last compile that completed; null while a compile
    /// runs, and after one that did not complete.
    std::unique_ptr<CompilationUnit> unit;

    // Signaled when the first compilation finishes. Feature handlers
    // co_await this before accessing the AST.
    kota::event ast_ready{false};

    // Compilation context (from CompileParams)
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    std::vector<std::uint8_t> open_conditionals;
    std::vector<std::uint32_t> preamble_inactive_regions;

    /// The preamble share from the compile params, then the own scan past
    /// the PCH bound, seeded by the conditional stack the preamble left
    /// open.
    std::vector<std::uint32_t> inactive_regions() {
        auto regions = preamble_inactive_regions;
        auto scan = feature::inactive_regions(*unit, open_conditionals, pch.second);
        regions.insert(regions.end(), scan.regions.begin(), scan.regions.end());
        return regions;
    }

    // Per-document serialization mutex
    kota::mutex strand;

    // Requests holding the entry: a compile queued on or running under the
    // strand, a query waiting for or reading the AST. The LRU passes such
    // an entry over — evicting it would tell the master the document is
    // gone before the compile's own reply, and the master's recompile
    // would evict the next one.
    unsigned pending = 0;
};

/// A request's hold on a document entry: keeps it alive through an Evict,
/// and counts in DocumentEntry::pending. Taken as the request is
/// dispatched, then moved into the task that serves it; a moved-from guard
/// holds nothing.
struct [[nodiscard]] PendingGuard {
    std::shared_ptr<DocumentEntry> doc;

    explicit PendingGuard(std::shared_ptr<DocumentEntry> entry) : doc(std::move(entry)) {
        doc->pending += 1;
    }

    PendingGuard(PendingGuard&&) noexcept = default;
    PendingGuard& operator=(PendingGuard&&) = delete;

    ~PendingGuard() {
        if(!doc) {
            return;
        }
        doc->pending -= 1;
        // The last hold on an evicted entry frees its AST: return the memory
        // as the eviction would have.
        if(doc.use_count() == 1) {
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
    PendingGuard pending;

    explicit CompileGuard(std::shared_ptr<DocumentEntry> entry) : pending(std::move(entry)) {}

    CompileGuard(CompileGuard&&) noexcept = default;
    CompileGuard& operator=(CompileGuard&&) = delete;

    ~CompileGuard() {
        if(pending.doc) {
            pending.doc->ast_ready.set();
        }
    }
};

class StatefulWorker {
    ipc::BincodePeer& peer;
    std::size_t max_documents;

    llvm::StringMap<std::shared_ptr<DocumentEntry>> documents;

    // LRU tracking — owns keys so they don't dangle after request handler returns
    std::list<std::string> lru;
    llvm::StringMap<std::list<std::string>::iterator> lru_index;

    void touch_lru(llvm::StringRef path) {
        auto it = lru_index.find(path);
        if(it != lru_index.end()) {
            lru.erase(it->second);
        }
        lru.emplace_front(path.str());
        lru_index[path] = lru.begin();
    }

    /// Evict least recently used idle documents down to the cap. Busy
    /// ones stay, so the cap is exceeded while more documents than it are
    /// in flight at once; the calls after their requests finish catch up.
    void shrink_if_over_limit() {
        bool evicted = false;
        auto it = lru.end();
        while(documents.size() > max_documents && it != lru.begin()) {
            it = std::prev(it);
            if(documents.lookup(*it)->pending > 0) {
                continue;
            }
            auto path = *it;
            LOG_DEBUG("Evicting document: {}", path);
            peer.send_notification(worker::EvictedParams{path});
            documents.erase(path);
            lru_index.erase(path);
            it = lru.erase(it);
            evicted = true;
        }
        if(evicted) {
            release_free_memory();
        }
    }

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
    /// `missing` if the AST is unusable, document_unloaded if the document
    /// is not held at all — an eviction the master has not learned of yet,
    /// which must not pass for an empty answer. `kind` discriminates the
    /// perf series: query kinds have very different costs and must not
    /// collapse into one distribution.
    template <typename Params, typename R, typename F>
    kota::task<R, ipc::Error>
        with_ast_or(llvm::StringRef kind, const Params& params, R missing, F fn) {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            return kota::outcome_error(
                ipc::Error{worker::dispatch_errc::document_unloaded, "Document is not loaded"});
        }
        touch_lru(params.path);
        return serve_ast(kind, PendingGuard(it->second), params, std::move(missing), std::move(fn));
    }

    template <typename Params, typename R, typename F>
    kota::task<R, ipc::Error> serve_ast(llvm::StringRef kind,
                                        PendingGuard pending,
                                        const Params& params,
                                        R missing,
                                        F fn) {
        auto& doc = *pending.doc;
        ScopedTimer timer;
        co_await doc.ast_ready.wait();
        auto strand = co_await doc.strand.scoped_lock();
        auto acquire_ms = timer.ms_f();
        double compute_ms = 0;

        // The frame stays alive until fn returns even when the handler is
        // cancelled mid-await, so the by-reference captures are safe; the
        // hook just lets a cancelled query return its missing value instead
        // of walking the whole AST for a result nobody will read.
        std::atomic<bool> cancelled{false};
        auto result = co_await kota::queue(
            [&]() -> R {
                if(cancelled.load(std::memory_order_relaxed)) {
                    return std::move(missing);
                }
                if(!doc.unit)
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
        shrink_if_over_limit();
        co_return result;
    }

    RequestResult<worker::CompileParams> serve_compile(CompileGuard guard,
                                                       const worker::CompileParams& params);

public:
    StatefulWorker(ipc::BincodePeer& peer, std::size_t max_documents) :
        peer(peer), max_documents(max_documents) {}

    void register_handlers();
};

RequestResult<worker::CompileParams>
    StatefulWorker::serve_compile(CompileGuard guard, const worker::CompileParams& params) {
    auto doc = guard.pending.doc;
    auto strand = co_await doc->strand.scoped_lock();

    // Copy params to doc AFTER acquiring the strand lock, so that
    // concurrent Compile requests waiting on the strand don't
    // overwrite our fields before we use them.
    doc->version = params.version;
    doc->text = params.text;
    doc->directory = params.directory;
    doc->arguments = params.arguments;
    doc->pch = params.pch;
    doc->open_conditionals = params.open_conditionals;
    doc->preamble_inactive_regions = params.preamble_inactive_regions;
    doc->pcms = params.pcms;

    // The old AST describes the text this request just replaced: drop
    // it before the cancellable await, or a cancellation arriving while
    // the work is still queued would wake waiters with the previous
    // unit installed next to the new buffer — with_ast_or would serve
    // stale offsets as current. Waiters observe no unit and return their
    // missing value until a compile finishes.
    doc->unit.reset();

    // The parse itself is interruptible: CompilationParams::stop is
    // polled after every top-level declaration, so the queue hook
    // (fired when this frame is cancelled, as the master's interrupt
    // does) reaches into the middle of the AST build instead of
    // waiting for it to finish; an interrupted unit reports
    // !completed() and the phases after it are skipped like any other
    // incomplete compile.
    auto stop = std::make_shared<std::atomic_bool>(false);
    auto compile_result = co_await kota::queue(
        [&]() -> worker::CompileResult {
            CrashScope crash_scope(worker::crash_tag(params));
            ScopedTimer timer;

            CompilationParams cp{
                .kind = CompilationKind::Content,
                .input = {.arguments = doc->arguments,
                          .directory = doc->directory,
                          .workspace = params.workspace,
                          .vfs = llvm::makeIntrusiveRefCnt<vfs::View>(params.hidden)},
                .tidy = params.tidy,
                .skip_bodies = params.skip_bodies,
                .preamble_inactive_regions = params.preamble_inactive_regions,
                .stop = stop,
            };
            use_artifacts(cp.input, doc->pch, doc->pcms);
            cp.input.add_focus(params.path, doc->text);
            cp.input.add_synthesized(params.synthesized);

            auto unit = compile(std::move(cp));

            worker::CompileResult result;
            result.version = doc->version;

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
            // retracted pair per round and self-corrects on the retry.
            if((!unit->completed() || unit->fatal_error()) && !doc->pch.first.empty()) {
                auto points_at_pch = [&](const Diagnostic& diag) {
                    llvm::StringRef message = diag.message;
                    if(message.contains(doc->pch.first)) {
                        return true;
                    }
                    if(!diag.id.is_deserialization_error()) {
                        return false;
                    }
                    return ranges::none_of(doc->pcms, [&](auto& entry) {
                        return message.contains(entry.second);
                    });
                };
                result.pch_suspect = ranges::any_of(unit->diagnostics(), [&](auto& diag) {
                    return points_at_pch(diag) || ranges::any_of(diag.notes, points_at_pch);
                });
            }

            std::optional<std::string> index_error;
            if(unit->completed() && !stop->load(std::memory_order_relaxed)) {
                result.build_at = unit->build_at().count();
                result.deps = unit->deps();

                index_error = hand_over_index(
                    index::build_tu_index(*unit,
                                          {.known_instantiations = params.known_instantiations,
                                           .standard_library_instantiations =
                                               params.standard_library_instantiations}),
                    params.index_output_path,
                    result);
            }

            if(unit->completed()) {
                auto diags = feature::diagnostics(*unit);
                if(index_error) {
                    diags.push_back(feature::index_unavailable(*index_error));
                }
                LOG_INFO("Compile done: path={}, {}ms, {} diags, fatal={}",
                         params.path,
                         timer.ms(),
                         diags.size(),
                         unit->fatal_error());
                result.diagnostics = std::move(diags);
                doc->unit = std::move(unit);
            } else {
                result.status = unit->setup_fail() ? worker::CompileStatus::SetupFail
                                                   : worker::CompileStatus::Cancelled;
                if(unit->setup_fail()) {
                    result.diagnostics = feature::diagnostics(*unit);
                }
                LOG_WARN("Compile incomplete: path={}, {}ms, setup_fail={}",
                         params.path,
                         timer.ms(),
                         unit->setup_fail());
                // An incomplete unit can never serve a query, yet it pins
                // the consumed artifacts — on Windows a mapped PCH cannot
                // be replaced on disk, so holding it would block the
                // master's rebuild of a retracted pair.
                unit.reset();
            }
            release_free_memory();
            return result;
        },
        [stop] { stop->store(true, std::memory_order_relaxed); });

    shrink_if_over_limit();

    co_return compile_result;
}

void StatefulWorker::register_handlers() {
    // === Compile ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CompileParams& params) -> RequestResult<worker::CompileParams> {
            LOG_INFO("Compile request: path={}, version={}", params.path, params.version);
            CompileGuard guard(get_or_create(params.path));
            touch_lru(params.path);
            return serve_compile(std::move(guard), params);
        });

    // === DocumentLink ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentLinkParams& params)
                        -> RequestResult<worker::DocumentLinkParams> {
        return with_ast_or("DocumentLink",
                           params,
                           std::vector<index::DocumentLink>{},
                           [&](DocumentEntry& doc) { return feature::document_links(*doc.unit); });
    });

    // === FoldingRange ===
    peer.on_request([this](RequestContext& ctx, const worker::FoldingRangeParams& params)
                        -> RequestResult<worker::FoldingRangeParams> {
        return with_ast_or(
            "FoldingRange",
            params,
            std::optional<std::vector<feature::FoldingRange>>{},
            [&](DocumentEntry& doc) { return std::optional(feature::folding_ranges(*doc.unit)); });
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
                    feature::semantic_tokens(*doc.unit, regions, params.config.semantic_tokens));
            });
    });

    // === DocumentSymbol ===
    peer.on_request([this](RequestContext& ctx, const worker::DocumentSymbolParams& params)
                        -> RequestResult<worker::DocumentSymbolParams> {
        return with_ast_or(
            "DocumentSymbol",
            params,
            std::optional<std::vector<feature::DocumentSymbol>>{},
            [&](DocumentEntry& doc) {
                auto regions = doc.inactive_regions();
                return std::optional(
                    feature::document_symbols(*doc.unit, regions, params.config.document_symbols));
            });
    });

    // === CodeAction ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CodeActionParams& params) -> RequestResult<worker::CodeActionParams> {
            return with_ast_or(
                "CodeAction",
                params,
                std::vector<feature::CodeAction>{},
                [&](DocumentEntry& doc) { return feature::code_actions(*doc.unit, params.range); });
        });

    // === SelectionRange ===
    peer.on_request([this](RequestContext& ctx, const worker::SelectionRangeParams& params)
                        -> RequestResult<worker::SelectionRangeParams> {
        return with_ast_or("SelectionRange",
                           params,
                           std::vector<std::vector<LocalSourceRange>>{},
                           [&](DocumentEntry& doc) {
                               return feature::selection_ranges(*doc.unit, params.offsets);
                           });
    });

    // === InlayHint ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::InlayHintParams& params) -> RequestResult<worker::InlayHintParams> {
            return with_ast_or(
                "InlayHint",
                params,
                std::vector<feature::InlayHint>{},
                [&](DocumentEntry& doc) {
                    return feature::inlay_hints(*doc.unit, params.range, params.config.inlay_hints);
                });
        });

    // === Evict ===
    peer.on_notification([this](const worker::EvictParams& params) {
        LOG_DEBUG("Evict notification: path={}", params.path);

        auto it = lru_index.find(params.path);
        if(it != lru_index.end()) {
            lru.erase(it->second);
            lru_index.erase(it);
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
                                   return feature::hover_info(*doc.unit,
                                                              params.offset,
                                                              params.config.hover,
                                                              params.config.documentation);
                               });
        });
}

int run_stateful_worker_mode(const std::string& worker_name,
                             const std::string& log_dir,
                             std::size_t max_documents) {
    logging::stderr_logger(worker_name, logging::options);
    if(!log_dir.empty()) {
        // File only: worker stderr is reserved for crash/unexpected output,
        // which the master relays into its own log (see logging taxonomy).
        logging::file_logger(worker_name, log_dir, logging::options, /*mirror_stderr=*/false);
    }

    LOG_INFO("Starting stateful worker");
    install_crash_report();

    kota::event_loop loop;

    auto transport_result = ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }
    (*transport_result)->set_remote_max_payload(ipc::default_max_payload);

    ipc::BincodePeer peer(loop, std::move(*transport_result));
    install_test_gates(peer, loop);

    StatefulWorker worker(peer, max_documents);
    worker.register_handlers();

    LOG_INFO("Stateful worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateful worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
