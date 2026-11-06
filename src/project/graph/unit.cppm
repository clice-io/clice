module clice:project.graph.unit;

import :compile.compilation;
import :feature.diagnostics;
import :support.prelude;
import :vfs.cache_store;
import :worker.protocol;
import :worker.scheduler;

namespace clice {

/// What one compile of a unit on a stateless worker serves: the full
/// index merged into the project's index, a clang-tidy pass, or both from
/// the single parse. The background index asks for {index}; `clice lint`
/// for {tidy} or {index, tidy}. Each process hosts one consumer of a unit's
/// compiles, so a late joiner asking for something else cannot arise — that
/// protocol comes with the first concurrent consumer (in-server background
/// checks).
///
/// A unit is a node of the task graph whose attempt is the compile; the
/// project owns its policy: command resolution, edges to the module PCMs
/// and to the precompiled header the build declares, the worker dispatch,
/// and the merge with its outdated check, whose report it takes
/// (Project::take_report). The background index owns the queue and the
/// requeue budget; it reads the compile's result to update them.
struct UnitRequest {
    bool index = false;

    /// The errors the compile reports, in the result.
    bool errors = false;

    /// The diagnostics the editor takes from this compile, in the result
    /// (worker::CompileUnitResult::background_diagnostics): none but for a
    /// file of its own database entry, which the editor compiles as this
    /// compile does.
    bool background_diagnostics = false;

    /// The file's unused includes among those diagnostics.
    bool unused_includes = false;

    /// A tidy pass under this frozen configuration.
    std::optional<tidy::TidyParams> tidy;

    /// Whether the index the compile built no longer describes the unit when
    /// it would merge (ReindexQueue::outdated): a newer content-level
    /// invalidation during the build, or a removal clearing the entry. The
    /// merge is dropped then; the follow-up attempt redoes it. Null when every
    /// result merges.
    std::function<bool()> outdated;
};

/// How a compile's TU index comes from its worker (Project::request_index):
/// in the reply, or in a transfer file when too large for a message.
struct IndexTransfer {
    /// The index the reply carries, or the transfer file holds when the
    /// worker wrote it there; why that cannot be read, else.
    std::expected<llvm::StringRef, std::string> receive(std::span<const std::byte> data,
                                                        bool in_file);

    /// The store's file the worker writes an index too large for a message
    /// to, removed with this; none without a store.
    std::optional<CacheStore::PendingEntry> file;

    /// The file, mapped once received. Declared after `file`: the mapping
    /// must close before the entry removes the file, which Windows refuses
    /// while it is mapped.
    fs::Mapping mapping;
};

/// What a unit's compiles in this session showed, ahead of what its index
/// records: what the next compile must send or leave out, and what they
/// measured.
struct UnitHistory {
    /// The last result named a variant by hash that the project index did
    /// not hold for its file: the next compile sends every section's bytes,
    /// so the retry cannot fail the same way.
    bool send_in_full = false;

    /// The key of the target's precompiled header the last compile crashed
    /// its worker over: the next compile leaves it out. Empty for none.
    std::string crashed_over_pch;

    /// The peak of the last finished compile, at least what a compile killed
    /// for memory before it reached, and the memory one killed since
    /// reached: ahead of the manifest's peak, which only an index that merged
    /// records. None while nothing was measured.
    std::optional<RequestCost> cost;
};

/// What one compile of a unit did.
struct UnitResult {
    enum class Status : std::uint8_t {
        /// The compile produced what was asked (the index merged into the
        /// project index, the tidy findings arrived in the result).
        Completed,
        /// Deliberately produced nothing: the result is outdated, or the TU
        /// has no real command but keeps its last-known index.
        Skipped,
        /// Terminal failure on current content: the worker rejected the TU,
        /// returned an empty or unverifiable result, the merge was
        /// rejected, or the TU has no real command and no surviving index.
        Failed,
        /// The compile killed its worker: the worker named it, or it ran
        /// past the pool's deadline. The same bytes would again.
        Crashed,
        /// The worker died under it for some other reason; requeue-worthy
        /// on the budget.
        Lost,
        /// Preempted (deliberate cancellation, or an outage the pool will
        /// revive from): budget-free requeue.
        Preempted,
        /// Its worker's memory ran out beside other work: budget-free
        /// requeue, its next compile expected to take at least `memory`.
        OutOfMemory,
        /// Its worker's memory ran out with no other work beside it, at
        /// `memory`: the TU does not fit this machine, and the same bytes
        /// would not again.
        TooBig,
    };

    Status status = Status::Failed;

    /// Findings of the tidy pass (UnitRequest::tidy).
    std::vector<worker::TidyDiagnostic> tidy_diagnostics;

    /// Failure detail for the requester's logs.
    std::string error;

    /// OutOfMemory and TooBig: the memory the worker reached.
    std::uint64_t memory = 0;

    /// The errors a completed compile reported (UnitRequest::errors).
    std::vector<worker::LocatedMessage> errors;

    /// See worker::CompileUnitResult::background_diagnostics
    /// (UnitRequest::background_diagnostics).
    std::vector<feature::FileDiagnostic> background_diagnostics;

    /// The hash of the text background_diagnostics describe.
    std::uint64_t text_hash = 0;

    struct Perf {
        std::size_t bytes = 0;

        long long index_ms = 0;

        long long merge_ms = 0;
    } perf;
};

}  // namespace clice
