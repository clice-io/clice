module clice:project.pcm;

import :project.command;
import :project.graph;
import :support.failure_record;
import :support.prelude;
import :vfs.ids;

namespace clice {

struct Project;

/// A scan's module dependencies: the full durable edge set of a unit, a
/// node per module unit it imports and one unresolved import per name no
/// unit provides. A module unit's PCM is one node per unit in the task
/// graph, edged to the modules it imports; one round is one PCM build (or
/// the revalidation of the last one). The project owns the PCM policy:
/// cache key computation and hit checks, store commits, the modules' build
/// failures (Project::failures), and the cooperative response to advisory
/// cancellation (a voided round tells its worker to stop and still reports
/// the real outcome — contract 2). The graph owns identity, edges, rounds
/// and interest.
struct ModuleDeps {
    llvm::SmallVector<NodeId, 8> declared;

    /// Where each of `declared` is imported (see ScanResult::import_offsets);
    /// an implementation unit's import of its own module sits at UINT32_MAX,
    /// past any preamble.
    llvm::SmallVector<std::uint32_t, 8> offsets;

    /// The unit declares a module.
    bool module_unit = false;

    /// The module units of `declared`: what a consumer waits on.
    llvm::SmallVector<Fid> resolved() const;

    /// A parse of the unit meets a module declaration, which no declaration
    /// may precede, or a PCM, which names the precompiled header it was
    /// built over to its importers: over the precompiled header its command
    /// declares, it reads the one every PCM is built over.
    bool modular() const;
};

/// A unit's last precise import scan and what it ran against.
struct ImportScan {
    std::uint64_t directives = 0;

    /// The preamble bound of the scanned text: the import offsets are
    /// positions, which an edit moving the preamble without changing a
    /// directive moves too.
    std::uint32_t bound = 0;

    std::uint64_t arguments = 0;

    std::uint64_t epoch = 0;

    std::vector<std::string> imports;

    std::vector<std::uint32_t> offsets;

    bool module_unit = false;
};

/// Why a module did not build: `chain` runs from it through the imports
/// that failed in turn to the module whose own build failed, on `error`,
/// its first error — or, with `cycle`, to the import that closes a cycle
/// back to chain[*cycle].
struct ModuleBuildFailure {
    llvm::SmallVector<Fid> chain;

    std::string error;

    std::optional<std::size_t> cycle;
};

/// The graph identity of an import that resolves to nothing: a node that
/// never runs a round and exists only to be edged at. When the name's
/// first provider appears, provider_appeared() updates it and the ordinary
/// cascade reaches every consumer whose scan declared the edge — no side
/// bookkeeping of who failed against the name. The high bit keeps the key
/// space disjoint from path_ids.
inline NodeId unresolved_import(llvm::StringRef name) {
    return {NodeKind::PCM, (1ull << 63) | (llvm::xxh3_64bits(name) >> 1)};
}

inline bool is_unresolved_import(NodeId id) {
    return id.kind == NodeKind::PCM && (id.key >> 63) != 0;
}

/// Re-validate on-disk PCM blobs and build the modules a request imports
/// that compiles under `arguments` with `content` as the focus file and the
/// resolution's synthesized context served from memory (the stateless
/// requests' builds — the scan must see the buffer's imports under the
/// request's command). Building a dependency can itself evict another clean
/// module's PCM under budget pressure, which reopens the window the
/// revalidation just closed — hence the bounded retry until the set is
/// stable. A module that did not build is the compile's to report, as the
/// stateful compile does; none only when the graph shut down.
async::task<std::optional<ModuleDeps>> build_imports(Project& project,
                                                     Fid path_id,
                                                     const Resolution& resolution,
                                                     llvm::ArrayRef<const char*> arguments,
                                                     llvm::StringRef directory,
                                                     llvm::StringRef content);

/// One pass of the on-disk revalidation: LRU eviction can remove a blob, or
/// the PCH it was built over, while its node is still clean, so evicted
/// units lose their path and are re-dirtied instead of handing clang a
/// dangling path — and so are the units importing them, directly or not: a
/// clean module promises every module below it built (see module_closure).
/// Returns whether anything was evicted.
///
/// FIXME: this stats every built PCM (pcm_builds) on every compile that
/// imports a module, even when nothing was evicted: slow for a large
/// modular project whose cache sits on a network filesystem.
bool revalidate_pcms(Project& project);

/// Commit `deps`, a scan of the file of the round's node `self`, as the
/// node's durable edges (TaskGraph::declare), and reference its unresolved
/// imports from the round, whose successful commit replaces the
/// declaration with its candidates. The scan outlives the round: a unit
/// whose build fails stays cascade-reachable from its imports, so fixing or
/// providing an import re-dirties it — the include reverse map carries no
/// import edges, and the index records only imports that resolved
/// (ProjectIndex::importers).
void declare_imports(Project& project, RoundContext& ctx, NodeId self, const ModuleDeps& deps);

/// Whether the graph has a node for this module unit (it was built or
/// depended on before).
bool pcm_tracked(const Project& project, Fid path_id);

/// The modules a compile importing `modules` reads, once it waited on them:
/// they and, below each that built, the modules it imports in turn — clang
/// loads every module below an import, and a built module's imports are
/// built too.
llvm::SetVector<Fid> module_closure(const Project& project, llvm::ArrayRef<Fid> modules);

/// Mark a module unit and its transitive importers dirty, voiding in-flight
/// rounds: their next rounds check their builds again, and a build whose
/// key and inputs still hold is kept — the single write point for PCM
/// content invalidation. A product-only loss (cache eviction) goes through
/// revalidate_pcms instead, which re-dirties without voiding in-flight
/// rounds: what they read is unchanged. Returns the dirtied nodes' files,
/// of every kind.
llvm::SmallVector<Fid> invalidate_module(Project& project, Fid path_id);

/// A closed document's buffer scans no more.
void forget_buffer_scan(Project& project, Fid path_id);

/// The crash of the module's last build, while what it is known to have
/// read holds: every importer that finds it books it against its own
/// document instead of burning a worker of its own.
const FailureRecord* build_crash(Project& project, Fid module);

/// Why the module's last round failed, when it failed on the user's code;
/// nullopt for any other failure (a crash, an outage).
std::optional<ModuleBuildFailure> module_build_failure(const Project& project, Fid module);

/// Forget the module's failed build, a crash or errors in the user's code,
/// so the next round builds it again: an importer whose own record lets it
/// retry, or that was saved, retries it. A failed build is otherwise kept
/// until its key or one of its inputs changes. Returns whether it had
/// failed.
bool forget_build_failure(Project& project, Fid module);

/// A module name gained a provider it lacked: void the dependents of its
/// unresolved import (their keys name the import as unresolved, so their
/// next rounds rebuild) and return them for serving-side treatment.
llvm::SmallVector<NodeId> provider_appeared(Project& project, llvm::StringRef name);

/// Scan a module unit's disk text for its direct module dependencies under
/// the command its own build resolves (lazy, on every use — a re-resolve is
/// inherent, so a CDB or import change is always seen by the next round).
/// Consumers declare the full edge set and wait on the resolved subset.
async::task<ModuleDeps> direct_deps(Project& project, Fid path_id);

/// The already-resolved-command variant: scans under exactly the arguments
/// the caller will compile with, rendered by `resolution`. The AST path
/// uses it so a context choice or donated header host cannot diverge
/// between the scan and the parse — the path_id variant re-picks a CDB
/// entry, which is only right for whole-TU runs on real commands. The
/// header context the arguments name is served to the scan from memory. An
/// engaged `content` scans it in place of the file's on-disk text — even
/// when empty (an open buffer's imports count before they are saved, and
/// an emptied buffer has none).
///
/// The scan is a preprocessor run over the whole unit: it runs on the
/// thread pool, the event loop only resolves the names it found. A unit
/// that can import nothing pays none: its own text has no module syntax,
/// neither it nor the host whose command it borrows reaches an import
/// candidate, and a buffer's directives are the ones the dependency scan
/// saw. A scan is reused while its arguments and the project's disk state
/// (context_epoch) stay the same, a buffer's while its directive stream
/// does too.
async::task<ModuleDeps> direct_deps(Project& project,
                                    Fid path_id,
                                    const Resolution& resolution,
                                    llvm::ArrayRef<const char*> arguments,
                                    llvm::StringRef directory,
                                    std::optional<llvm::StringRef> content);

/// One round of a PCM node, the graph's runner for NodeKind::PCM: declare
/// the dependency edges, wait on the imports, keep the last build while it
/// holds, else dispatch a new one.
async::task<RoundOutcome> pcm_round(Project& project, RoundContext& ctx, NodeId id);

}  // namespace clice
