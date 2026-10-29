module clice:sched.builders.pch;

import :project.project;
import :sched.graph;
import :support.failure_record;
import :support.prelude;
import :vfs.file_system;
import :vfs.file_table;
import :worker.pool;

namespace clice {

/// Preamble artifacts (a PCH and its pch.idx index, committed as one
/// pair) as a task-graph family: one node per content-addressed pch_key,
/// no edges, one round = one revalidation or build. The facade is the
/// only surface consumers touch. The precompiled headers the build
/// declares (Build::pch_unit) are built here too, a whole file each, and
/// a preamble's PCH is built over the one its command reads.
///
/// The builder owns the shared side of the policy: the pair-atomic store
/// commit and cache metadata (a round runs to a real reply, so no
/// cancellation can split blob from metadata), the failures of the keys'
/// builds and consumption (Project::failures), and the cooperative
/// response to advisory cancellation. The adoption side stays with the
/// server: which session points at the key and the documents' failure
/// records — every consumer reads a failed acquisition's crash from
/// crashed() and books it for itself.
class PCHBuilder {
public:
    PCHBuilder(TaskGraph& graph, Project& project, WorkerPool& pool);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic build register their own runner under
    /// Family::PCH instead.
    void register_runner();

    /// One acquisition's request conditions: the artifact identity plus
    /// the inputs a build needs if this acquire ends up dispatching one.
    struct Request {
        std::string pch_key;
        std::string file;
        std::string directory;
        std::vector<std::string> arguments;
        /// See worker::BuildPCHParams::hidden.
        vfs::HiddenFiles hidden;
        /// The requesting document's buffer text; the preamble is its
        /// prefix and the build sends the whole buffer.
        std::string content;
        std::uint32_t preamble_bound = 0;
        /// See worker::BuildPCHParams::synthesized.
        SynthesizedFiles synthesized;
        /// See worker::BuildPCHParams::pcms.
        std::unordered_map<std::string, std::string> pcms;
        /// The built modules the preamble imports, whose inputs the PCH
        /// depends on.
        llvm::SmallVector<Fid> modules;
        /// The key of the precompiled header the build declares for the
        /// command (precompiled_header), built already: the preamble's PCH
        /// is built over it, and what it read is the preamble's input too.
        std::string base;
        /// See worker::BuildPCHParams::whole_file.
        bool whole_file = false;
        /// See worker::BuildPCHParams::skip_bodies.
        bool skip_bodies = false;
    };

    enum class Outcome : std::uint8_t {
        /// A fresh pair is registered under the key (revalidated or just
        /// built) — adopt it.
        Ready,
        Failed,
        /// The observed attempt ended without a result (advisory
        /// cancellation, shutdown): no adoption, but no failure result
        /// either.
        Preempted,
    };

    /// Join the key's round, spawning one when none is live. The spawning
    /// acquire is the dispatch owner: its inputs feed the build. One
    /// acquire observes exactly one attempt; retry policy stays with the
    /// caller.
    kota::task<Outcome> acquire(Request request);

    /// The request for clice's own build of the precompiled header the
    /// command of `unit` writes (Build::pch_unit): the unit's whole file,
    /// under that command. The editor's build skips function bodies, the
    /// index's keeps them: they are two PCHs.
    Request precompiled_header(const CommandRef& unit, bool skip_bodies);

    /// Whether the PCH registered under `key` read `file`: a parse of that
    /// very file cannot load it — clang refuses a PCH whose input the parse
    /// serves from a buffer, and the guard the PCH defines would skip the
    /// file's text.
    bool reads(llvm::StringRef key, Fid file) const;

    /// The stash-only half of acquire, for waits that must be recorded as
    /// graph edges: intern the key, re-dirty a stale clean node, and
    /// install the dispatch owner's inputs when the caller's join will
    /// spawn the round. The caller then waits through RoundContext::depend
    /// on the returned node — the only wait form a family round may use.
    NodeId prepare(Request request);

    /// The crash of the key's last build, while the inputs it is known to
    /// have read hold: a round's crash is recorded whatever became of the
    /// request that spawned it (contract 12), and every consumer that finds
    /// it books it against its own document instead of burning a worker of
    /// its own. One document's records cannot contain it alone: the
    /// artifact is shared.
    const FailureRecord* crashed(llvm::StringRef pch_key);

    /// Forget the key's failed build, a crash or errors in the user's code,
    /// so the next request builds it again: a consumer whose own record lets
    /// it retry, or that was saved, retries it, and one moving to another
    /// key releases it. Returns whether it had failed.
    bool forget_failure(llvm::StringRef pch_key) {
        return project.failures.pch_builds.forget(pch_key);
    }

    /// A complete, store-backed, deps-current pair is registered under
    /// the key. Non-const: a passing deps check may repair the snapshot's
    /// stat fast path in place.
    bool fresh(llvm::StringRef pch_key);

    /// A round for the key is in flight; its commit will republish over
    /// concurrent store retractions.
    bool building(llvm::StringRef pch_key) const;

    /// Retract a pair the frontend could not consume: remove both blobs
    /// from the store and drop the idle cache entry, so the next
    /// acquire misses and rebuilds instead of trusting corrupt bytes for
    /// the life of the store.
    void invalidate(llvm::StringRef pch_key);

    /// When reading a key's pairs stops it from building: two failures in a
    /// row within the cooldown. A stop must never be final: what fails may
    /// live in content the key cannot see (a header the preamble includes),
    /// so editing it cannot unlock the key. After the cooldown the key
    /// builds again, and stops again only after `limit` more failures —
    /// bounded burn, never a permanent decision, like slot revival.
    constexpr static FailurePolicy consume_policy{
        .limit = 2,
        .cooldown = std::chrono::minutes(5),
    };

    /// A parse reading the pair died or found it unreadable: retract it AND
    /// count a failure of its consumption — unless the pair was gone from
    /// the store by then: evicted under the parse, which says nothing of
    /// its bytes, or retracted by a failure counted already. Each
    /// successful rebuild clears the build's failure, so the consumption
    /// keeps a record of its own; enough failures stop the key and
    /// acquisitions fail fast, so consumers fall back to compiling without
    /// a PCH instead of rebuilding and failing forever over failing
    /// storage.
    void consume_failed(llvm::StringRef pch_key);

    /// A parse read the key's pair and completed: its consumption failures
    /// were transient — without this, two unrelated hiccups far apart
    /// would stop a key that rebuilds fine in between.
    void consume_succeeded(llvm::StringRef pch_key) {
        project.failures.pch_consumes.forget(pch_key);
    }

    /// Open the pch.idx index of a cached PCH. The single consumption
    /// gate for `.pch.idx` blobs: when the blob turns out unreadable, the
    /// on-disk pair is retracted from the store as well — otherwise every
    /// later session re-adopts the corrupt pair from the artifact records and
    /// silently degrades again. With the pair gone the next ensure_pch is
    /// a miss and rebuilds both halves. Loads count against the
    /// loaded-state budget (see enforce_loaded_budget).
    std::shared_ptr<index::TUIndex> preamble_state(llvm::StringRef pch_key);

    /// Unload pch.idx indexes beyond the budget (two per open document,
    /// plus 2), least recently used first. Without this every preamble key
    /// ever touched keeps its blob mapped for the server's lifetime — tens
    /// of MB per key on real projects, released by neither didClose nor
    /// store eviction. Unloading only drops the entry's reference:
    /// consumers holding the shared_ptr finish safely, and the next use
    /// reopens the blob from disk.
    void enforce_loaded_budget();

    /// The buffer text the dispatch owners' stashes hold (see KeyState).
    std::size_t stashed_bytes() const {
        std::size_t bytes = 0;
        for(auto& state: states) {
            bytes += state.inputs.content.size();
        }
        return bytes;
    }

    /// Open-document count provider, connected by the master. Sizes the
    /// loaded-state budget; unset (tests, tools) falls back to
    /// default_open_documents.
    std::function<std::size_t()> open_documents;

private:
    /// Forget an entry and retract the pairs it names.
    void drop(llvm::StringMap<PCHState>::iterator entry);

    /// One PCH round: run one attempt and retire the stash once a current
    /// round commits a result.
    kota::task<RoundOutcome> run(RoundContext& ctx, std::uint64_t key_id);

    /// One attempt: revalidate the registered pair, or dispatch a build
    /// with the dispatch owner's inputs and commit the pair.
    kota::task<RoundOutcome> attempt(RoundContext& ctx, std::uint64_t key_id);

    std::uint64_t intern(llvm::StringRef pch_key);

    static NodeId node(std::uint64_t key_id) {
        return {Family::PCH, key_id};
    }

    /// Per-key slot for the dispatch owner's stash; replaced by the
    /// spawning acquire, read by its round and any stale-retry rounds, and
    /// retired when a current round commits a result or nobody waits on
    /// the key any more.
    struct KeyState {
        Request inputs;
    };

    TaskGraph& graph;
    Project& project;
    WorkerPool& pool;

    /// Move a pch key to the front of the loaded-state LRU. Called
    /// whenever an entry's pch.idx index is opened or replaced.
    void touch_loaded_state(llvm::StringRef pch_key);

    /// Keys of pch_cache entries whose pch.idx index is currently loaded,
    /// most recently used first (see enforce_loaded_budget).
    llvm::SmallVector<std::string, 8> loaded_state_lru;

    llvm::StringMap<std::uint64_t> ids;
    std::vector<KeyState> states;
};

}  // namespace clice
