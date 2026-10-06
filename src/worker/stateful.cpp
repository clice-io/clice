module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

#include "kota/ipc/framing.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :index.tu_index;
import :support.logging;
import :worker.common;
import :worker.crash_report;
import :worker.protocol;
import :worker.stateful;

namespace clice {

using kota::ipc::RequestResult;
using RequestContext = kota::ipc::BincodePeer::RequestContext;
namespace protocol = kota::ipc::protocol;

namespace {

/// What a compile reports in place of an index too large for its reply.
protocol::Diagnostic index_too_large(std::size_t bytes) {
    return feature::file_warning(
        std::format("this file's index ({} MiB) is too large to send between clice processes; "
                    "features that read the file's own index, such as references within it, are "
                    "unavailable",
                    bytes / (1024 * 1024)));
}

}  // namespace

struct DocumentEntry {
    int version = 0;
    std::string text;
    bool has_ast = false;
    CompilationUnit unit{nullptr};

    // Signaled when the first compilation completes (has_ast becomes true).
    // Feature handlers co_await this before accessing the AST.
    kota::event ast_ready{false};

    // Compilation context (from CompileParams)
    std::string directory;
    std::vector<std::string> arguments;
    std::pair<std::string, uint32_t> pch;
    std::unordered_map<std::string, std::string> pcms;
    std::vector<std::uint8_t> open_conditionals;
    std::vector<std::uint32_t> preamble_inactive_regions;

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
        if(doc) {
            doc->pending -= 1;
        }
    }
};

/// A compile's hold on its document, which wakes the AST waiters when
/// released: at every exit of the compile, and when a cancel read with the
/// request destroys its task before it starts. An unset ast_ready would
/// hang every later query for the document (they observe has_ast == false
/// and return their missing value; the next Compile sets a real AST).
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
    kota::ipc::BincodePeer& peer;
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
    /// in flight at once; the calls after their requests settle catch up.
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
    kota::task<R, kota::ipc::Error>
        with_ast_or(llvm::StringRef kind, const Params& params, R missing, F fn) {
        auto it = documents.find(params.path);
        if(it == documents.end()) {
            return kota::outcome_error(kota::ipc::Error{worker::dispatch_errc::document_unloaded,
                                                        "Document is not loaded"});
        }
        touch_lru(params.path);
        return serve_ast(kind, PendingGuard(it->second), params, std::move(missing), std::move(fn));
    }

    template <typename Params, typename R, typename F>
    kota::task<R, kota::ipc::Error> serve_ast(llvm::StringRef kind,
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
                if(!doc.has_ast || (!doc.unit.completed() && !doc.unit.fatal_error()))
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

    /// Answers "null" if the AST is not usable.
    template <typename Params, typename F>
    kota::task<kota::codec::RawValue, kota::ipc::Error> with_ast(llvm::StringRef kind,
                                                                 const Params& params,
                                                                 F fn) {
        return with_ast_or(kind, params, kota::codec::RawValue{"null"}, std::move(fn));
    }

    RequestResult<worker::CompileParams> serve_compile(CompileGuard guard,
                                                       const worker::CompileParams& params);

public:
    StatefulWorker(kota::ipc::BincodePeer& peer, std::size_t max_documents) :
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
    // it before the cancellable await, or a cancellation landing while
    // the work is still queued would wake waiters with the previous
    // unit installed next to the new buffer — with_ast_or would serve
    // stale offsets as current. Waiters observe has_ast == false and
    // return their missing value until a compile lands.
    doc->has_ast = false;
    doc->unit = CompilationUnit(nullptr);

    // The parse itself is interruptible: CompilationParams::stop is
    // polled after every top-level declaration, so the queue hook
    // (fired when this frame is cancelled, as the master's interrupt
    // does) reaches into the middle of the AST build instead of
    // waiting for it to finish; an interrupted unit reports
    // !completed() and the phases after it are skipped like any other
    // incomplete compile. The document stays coherent at every early
    // exit (unit and has_ast are set together).
    auto stop = std::make_shared<std::atomic_bool>(false);
    auto compile_result = co_await kota::queue(
        [&]() -> worker::CompileResult {
            CrashScope crash_scope(worker::crash_tag(params));
            ScopedTimer timer;

            CompilationParams cp;
            cp.kind = CompilationKind::Content;
            fill_args(cp, doc->directory, doc->arguments);
            cp.workspace = params.workspace;
            use_artifacts(cp, doc->pch, doc->pcms);
            cp.add_remapped_file(params.path, doc->text);
            cp.add_synthesized(params.synthesized);
            cp.stop = stop;

            doc->unit = compile(cp);
            doc->has_ast = true;

            worker::CompileResult result;
            result.version = doc->version;

            // A failed parse that blames the consumed PCH: either a
            // diagnostic names the blob's path outright, or it is an
            // AST-deserialization error naming no other prebuilt input
            // (that family's messages do not reliably carry the path —
            // "malformed or corrupted precompiled file: 'Blob ends too
            // soon'"). User-code failures (missing include, modified
            // header, bad flags) match neither, so the master never
            // rebuilds an innocent shared PCH over a failure it did
            // not cause. An anonymous read error with PCMs in play is
            // ambiguous; blaming the PCH costs at most one retracted
            // pair per round and self-corrects on the retry.
            if(!doc->unit.completed() && !doc->pch.first.empty()) {
                result.pch_suspect = std::ranges::any_of(doc->unit.diagnostics(), [&](auto& diag) {
                    llvm::StringRef message = diag.message;
                    if(message.contains(doc->pch.first)) {
                        return true;
                    }
                    if(!diag.id.is_deserialization_error()) {
                        return false;
                    }
                    return std::ranges::none_of(doc->pcms, [&](auto& entry) {
                        return message.contains(entry.second);
                    });
                });
            }

            if(doc->unit.completed() && !stop->load(std::memory_order_relaxed)) {
                result.build_at = doc->unit.build_at().count();
                result.deps = doc->unit.deps();

                // Build index for main file only (main_file_only=true).
                result.tu_index_data = index::build_tu_index(doc->unit, true);
            }

            if(doc->unit.completed() || doc->unit.fatal_error()) {
                auto diags = feature::diagnostics(doc->unit);
                if(result.tu_index_data.size() > max_index_bytes()) {
                    diags.push_back(index_too_large(result.tu_index_data.size()));
                    result.tu_index_data.clear();
                }
                LOG_INFO("Compile done: path={}, {}ms, {} diags, fatal={}",
                         params.path,
                         timer.ms(),
                         diags.size(),
                         doc->unit.fatal_error());
                result.diagnostics = std::move(diags);
            } else {
                result.status = doc->unit.setup_fail() ? worker::CompileStatus::SetupFail
                                                       : worker::CompileStatus::Cancelled;
                LOG_WARN("Compile incomplete: path={}, {}ms, setup_fail={}",
                         params.path,
                         timer.ms(),
                         doc->unit.setup_fail());
            }

            // A unit that is neither complete nor a fatal-error result
            // can never serve a query (with_ast_or refuses it), yet it
            // pins the consumed artifacts — on Windows a mapped PCH
            // cannot be replaced on disk, so holding it would block
            // the master's rebuild of a retracted pair. Drop it last,
            // after every use of the unit above; queries observe
            // has_ast == false and return their missing value until a
            // compile lands.
            if(!doc->unit.completed() && !doc->unit.fatal_error()) {
                doc->unit = CompilationUnit(nullptr);
                doc->has_ast = false;
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
                           [&](DocumentEntry& doc) { return feature::document_links(doc.unit); });
    });

    // === FoldingRange ===
    peer.on_request([this](RequestContext& ctx, const worker::FoldingRangeParams& params)
                        -> RequestResult<worker::FoldingRangeParams> {
        return with_ast_or(
            "FoldingRange",
            params,
            std::optional<std::vector<feature::FoldingRange>>{},
            [&](DocumentEntry& doc) { return std::optional(feature::folding_ranges(doc.unit)); });
    });

    // === CodeAction ===
    peer.on_request(
        [this](RequestContext& ctx,
               const worker::CodeActionParams& params) -> RequestResult<worker::CodeActionParams> {
            return with_ast_or(
                "CodeAction",
                params,
                std::vector<feature::CodeAction>{},
                [&](DocumentEntry& doc) { return feature::code_actions(doc.unit, params.range); });
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
    });

    // === Query (hover, definition, semantic tokens, etc.) ===
    peer.on_request([this](RequestContext& ctx, const worker::QueryParams& params)
                        -> RequestResult<worker::QueryParams> {
        using K = worker::QueryKind;
        auto kind = kota::meta::enum_name(params.kind, "Unknown");
        switch(params.kind) {
            case K::Hover:
                return with_ast(kind, params, [&](DocumentEntry& doc) {
                    auto result = feature::hover(doc.unit, params.offset, params.config.hover);
                    return result ? to_raw(*result) : kota::codec::RawValue{"null"};
                });
            case K::SemanticTokens:
                return with_ast(kind, params, [&](DocumentEntry& doc) {
                    // The preamble share from the compile params, then
                    // the own scan past the PCH bound, seeded by the
                    // conditional stack the preamble left open.
                    auto regions = doc.preamble_inactive_regions;
                    auto scan =
                        feature::inactive_regions(doc.unit, doc.open_conditionals, doc.pch.second);
                    regions.insert(regions.end(), scan.regions.begin(), scan.regions.end());
                    return to_raw(feature::semantic_tokens(doc.unit,
                                                           regions,
                                                           feature::PositionEncoding::UTF16));
                });
            case K::InlayHints:
                return with_ast(kind, params, [&](DocumentEntry& doc) {
                    auto range = params.range;
                    if(range.begin == static_cast<uint32_t>(-1))
                        range = LocalSourceRange{0, static_cast<uint32_t>(doc.text.size())};
                    return to_raw(feature::inlay_hints(doc.unit,
                                                       range,
                                                       params.config.inlay_hints,
                                                       feature::PositionEncoding::UTF16));
                });
            case K::DocumentSymbol:
                return with_ast(kind, params, [&](DocumentEntry& doc) {
                    return to_raw(
                        feature::document_symbols(doc.unit, feature::PositionEncoding::UTF16));
                });
        }
        std::unreachable();
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
    prefer_as_oom_victim();

    kota::event_loop loop;

    auto transport_result = kota::ipc::StreamTransport::open_stdio(loop);
    if(!transport_result) {
        LOG_ERROR("Failed to open stdio transport");
        return 1;
    }
    (*transport_result)->set_remote_max_payload(kota::ipc::default_max_payload);

    kota::ipc::BincodePeer peer(loop, std::move(*transport_result));

    StatefulWorker worker(peer, max_documents);
    worker.register_handlers();

    LOG_INFO("Stateful worker ready, waiting for requests");
    loop.schedule(peer.run());
    auto ret = loop.run();
    LOG_INFO("Stateful worker exiting with code {}", ret);
    return ret;
}

}  // namespace clice
