module clice:project.pcm;

import :project.task_graph;
import :support.failure_record;
import :support.prelude;
import :vfs.file_table;
import :vfs.ids;

namespace clice {

/// The last build of a C++20 module unit that built: what it read and the
/// PCM it wrote. Shared across all files that import the module; a build
/// that failed is a failure record (PrecompiledFailures::pcm_builds).
struct PCMBuild {
    /// The build's identity: "{module}-{hash}" over the unit's path and
    /// canonical flags, the build configuration, and the key of every module
    /// it imports (an unresolved import by its name) — so a command change
    /// anywhere below the module changes its key too.
    std::string key;

    /// The PCM's name in the store: the key and a nonce of the build that
    /// produced it. A rebuild publishes under a new name, never over the
    /// old PCM, which a running compile may still read — and the record
    /// names the very bytes it describes, whatever a crash before the
    /// metadata flush or another writer of the store left behind.
    std::string blob;

    /// The PCM the latest build replaced, kept for one more build.
    std::string previous;

    /// The built PCM; empty while a rebuild runs, after a failure, and once
    /// it or the PCH it was built over left the store.
    std::string path;

    /// The PCH the PCM was built over, which its importers read with it;
    /// empty for none.
    std::string pch;

    /// What the build read and looked for, the inputs of the modules it
    /// imports included; none while a rebuild runs.
    Inputs inputs;
};

/// A module unit's build that failed: on the user's code, on an import that
/// failed in turn, or by crashing a worker.
struct ModuleFailure {
    /// The key of the build that failed (PCMBuild::key); empty when an
    /// import failed it and it never ran.
    std::string key;

    /// The build's first error, or how the worker it crashed died; empty
    /// when an import failed it.
    FailureRecord record;

    /// The import whose build failed, or that closed a cycle back to the
    /// unit; invalid when the unit's own build failed.
    Fid import;
};

/// A scan's module dependencies: the full durable edge set of a unit, a
/// node per module unit it imports and one unresolved import per name no
/// unit provides. A module unit's PCM is one node per unit in the task
/// graph, edged to the modules it imports; one attempt is one PCM build (or
/// the revalidation of the last one). The project owns the PCM policy:
/// cache key computation and hit checks, store commits, the modules' build
/// failures (Project::failures), and the cooperative response to advisory
/// cancellation (an overtaken attempt tells its worker to stop and still
/// reports the real result — contract 2). The graph owns identity, edges,
/// attempts and interest.
struct Imports {
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
/// never runs an attempt and exists only to be edged at. When the name's
/// first provider appears, provider_appeared() invalidates it and the ordinary
/// cascade reaches every consumer whose scan declared the edge — no side
/// bookkeeping of who failed against the name. The high bit keeps the key
/// space disjoint from path_ids.
inline NodeId unresolved_import(llvm::StringRef name) {
    return {NodeKind::PCM, (1ull << 63) | (llvm::xxh3_64bits(name) >> 1)};
}

inline bool is_unresolved_import(NodeId id) {
    return id.kind == NodeKind::PCM && (id.key >> 63) != 0;
}

/// The graph node of a module unit's PCM.
inline NodeId module_node(Fid module) {
    return {NodeKind::PCM, module.raw};
}

/// How many times a compile waits on its imports for all of them to be
/// present at once: a rebuild can itself evict another module under budget
/// pressure. Past that a module build fails unrecorded, and a parse fails
/// visibly on the missing file.
constexpr inline int import_passes = 3;

}  // namespace clice
