module;

#include "modules/prelude.h"

module clice:sched.families.pcm;

import :project.command_resolver;
import :project.project;
import :sched.graph;
import :worker.pool;

namespace clice {

/// C++20 module artifacts (PCM) as a task-graph family: one node per
/// module unit, edges to the modules it imports, one round = one PCM
/// build (or cache revalidation). The facade is the only surface
/// consumers touch — serve and batch never speak to the graph about PCM
/// nodes directly.
///
/// The family owns the PCM policy: cache key computation and hit checks,
/// store commits, the shared-artifact crash budget, and the cooperative
/// response to advisory cancellation (a voided round tells its worker to
/// stop and still reports the real outcome — contract 2). The graph owns
/// identity, edges, rounds and interest.
class PCMFamily {
public:
    PCMFamily(TaskGraph& graph, Project& project, CommandResolver& commands, WorkerPool& pool);

    /// Register the production runner. Tests that drive the facade
    /// against a synthetic topology register their own runner under
    /// Family::PCM instead.
    void register_runner();

    /// A scan's module dependencies, split by what a consumer does with
    /// them: `resolved` names module units to wait on; `declared` is the
    /// full durable edge set — resolved units' nodes plus one sentinel
    /// per unresolved name.
    struct ModuleDeps {
        llvm::SmallVector<Fid> resolved;
        llvm::SmallVector<NodeId, 8> declared;

        /// Where each of `declared` is imported (see
        /// ScanResult::import_offsets); an implementation unit's import of
        /// its own module comes past the end.
        llvm::SmallVector<std::uint32_t, 8> offsets;

        /// The unit declares a module.
        bool module_unit = false;
    };

    /// Re-validate on-disk PCM blobs and build the module dependencies of
    /// a request that compiles under `arguments` with `content` as the
    /// main file and the resolution's synthesized context served from
    /// memory (the forwarder's per-request builds — the scan must see the
    /// buffer's imports under the request's command). Building a
    /// dependency can itself evict another clean module's PCM under budget
    /// pressure, which reopens the window the revalidation just closed —
    /// hence the bounded retry until the set is stable. None when a
    /// dependency failed to build.
    kota::task<std::optional<ModuleDeps>> prepare_deps(Fid path_id,
                                                       const Resolution& resolution,
                                                       llvm::ArrayRef<const char*> arguments,
                                                       llvm::StringRef directory,
                                                       llvm::StringRef content);

    /// One pass of the on-disk revalidation: LRU eviction can remove a
    /// blob while its node is still clean, so evicted units are
    /// invalidated instead of handing clang a dangling path. Returns
    /// whether anything was evicted.
    ///
    /// FIXME: this scans every pcm_cache entry (one stat() per module) on
    /// every compile, even in steady state when nothing was evicted. For
    /// large modular projects on NFS this adds measurable latency.
    /// Consider having CacheStore notify on eviction or caching the scan
    /// result.
    bool revalidate_blobs();

    /// Whether the graph has a node for this module unit (it was built or
    /// depended on before).
    bool tracks(Fid path_id) const;

    /// Mark a module unit and its transitive importers dirty, voiding
    /// in-flight rounds and dropping their cached PCM state — the single
    /// write point for PCM content invalidation. Artifact-only loss
    /// (cache eviction) goes through revalidate_blobs' graph.mark_dirty
    /// instead: no cascade, importers' results still describe unchanged
    /// content. Returns the dirtied path_ids.
    llvm::SmallVector<Fid> invalidate(Fid path_id);

    /// Invoked after a PCM lands so background indexing can pick up the
    /// new artifact.
    std::function<void()> on_indexing_needed;

    /// Preprocessor passes direct_deps() ran.
    std::uint64_t import_scans = 0;

    /// A closed document's buffer scans no more.
    void forget_buffer(Fid path_id) {
        scan_memos.erase(path_id);
    }

    /// The graph identity of an import that resolves to nothing: a node
    /// that never runs a round and exists only to be edged at. When the
    /// name's first provider appears, provider_appeared() updates it and
    /// the ordinary cascade reaches every consumer whose scan declared
    /// the edge — no side bookkeeping of who failed against the name.
    /// The high bit keeps the key space disjoint from path_ids.
    static NodeId unresolved_node(llvm::StringRef name) {
        return {Family::PCM, (1ull << 63) | (llvm::xxh3_64bits(name) >> 1)};
    }

    /// The death the module's last build caused, while the module's
    /// content is still what crashed: every importer that finds it books
    /// it against its own document instead of burning a worker of its own.
    const kota::ipc::Error* crashed(Fid module);

    /// An importer holding a license to retry (see server/quarantine.h)
    /// lifts the module's refusal.
    void forgive(Fid module) {
        build_crashes.erase(module);
    }

    /// An importer's save retries the module's failed build (see
    /// build_failures). Returns whether it had failed.
    bool forget_failure(Fid module) {
        return build_failures.erase(module);
    }

    /// Whether a node is an unresolved-import sentinel.
    static bool is_unresolved(NodeId id) {
        return id.family == Family::PCM && (id.key >> 63) != 0;
    }

    /// A module name gained a provider it lacked: void the sentinel's
    /// dependents (dropping dirtied units' cached PCM state on the way)
    /// and return them for serving-side treatment.
    llvm::SmallVector<NodeId> provider_appeared(llvm::StringRef name);

    /// Scan a module unit's disk text for its direct module dependencies
    /// under the command its own build resolves (lazy, on every use — a
    /// re-resolve is inherent, so a CDB or import change is always seen
    /// by the next round). Consumers declare the full edge set and wait
    /// on the resolved subset.
    kota::task<ModuleDeps> direct_deps(Fid path_id);

    /// The already-resolved-command flavor: scans under exactly the
    /// arguments the caller will compile with, rendered by `resolution`.
    /// The AST path uses it so a context choice or donated header host
    /// cannot diverge between the scan and the parse — the path_id flavor
    /// re-picks a CDB entry, which is only right for whole-TU runs on real
    /// commands. The header context the arguments name is served to the
    /// scan from memory. An engaged `content` scans it in place of the
    /// file's on-disk text — even when empty (an open buffer's imports
    /// count before they are saved, and an emptied buffer has none).
    ///
    /// The scan is a preprocessor run over the whole unit: it runs on the
    /// thread pool, the event loop only resolves the names it found. A
    /// unit that can import nothing pays none: its own text has no module
    /// syntax, neither it nor the host whose command it borrows reaches an
    /// import candidate, and a buffer's directives are the ones the
    /// dependency scan saw. A buffer's scan is reused while its directive
    /// stream, its arguments and the project's disk state (context_epoch)
    /// stay the same.
    kota::task<ModuleDeps> direct_deps(Fid path_id,
                                       const Resolution& resolution,
                                       llvm::ArrayRef<const char*> arguments,
                                       llvm::StringRef directory,
                                       std::optional<llvm::StringRef> content);

private:
    /// Commit the scan's full edge set as the unit's durable edges (see
    /// TaskGraph::declare).
    void declare_deps(Fid path_id, llvm::ArrayRef<NodeId> deps);

    /// One PCM round: declare dependency edges, revalidate the cache, and
    /// dispatch the build.
    kota::task<RoundOutcome> run(RoundContext& ctx, Fid path_id);

    static NodeId node(Fid path_id) {
        return {Family::PCM, path_id.raw};
    }

    /// A buffer's last precise scan and what it ran against.
    struct ScanMemo {
        std::uint64_t directives = 0;
        std::uint64_t arguments = 0;
        std::uint64_t epoch = 0;
        std::vector<std::string> imports;
        std::vector<std::uint32_t> offsets;
        bool module_unit = false;
    };

    TaskGraph& graph;
    Project& project;
    CommandResolver& commands;
    WorkerPool& pool;

    llvm::DenseMap<Fid, ScanMemo> scan_memos;

    /// A module build that killed a worker: the content it was built from
    /// and the death.
    struct Crash {
        std::uint64_t content = 0;
        kota::ipc::Error error;
    };

    std::uint64_t content_hash(Fid module);

    /// The modules whose build killed a worker (see crashed): refused
    /// until an importer forgives them or their content changes. One
    /// document's quarantine cannot contain it alone: every importer would
    /// burn a worker of its own.
    llvm::DenseMap<Fid, Crash> build_crashes;

    /// A module build that failed on errors in the user's code: the cache
    /// key it ran under and what it read and looked for, its own source
    /// and the modules it imported included.
    struct Failure {
        std::string key;
        DepsSnapshot deps;
    };

    /// The modules whose build failed (see Failure): refused until their
    /// command or one of those files changes, an import that resolved to
    /// nothing gains a provider, or an importer is saved. Rebuilt on unchanged inputs, every
    /// compile and completion of every importer would pay for the failing
    /// build first.
    llvm::DenseMap<Fid, Failure> build_failures;
};

}  // namespace clice
