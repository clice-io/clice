module clice:project.pcm;

import :project.precompiled;
import :project.task_graph;
import :support.failure_record;
import :support.prelude;
import :vfs.file_table;
import :vfs.ids;

namespace clice {

/// A module unit's build that failed: on the user's code, on an import that
/// failed in turn, or by crashing a worker.
struct ModuleBuildFailed {
    /// The key of the build that failed (PCMEntry::key); empty when an
    /// import failed it and it never ran.
    std::string key;

    /// The build's first error, or how the worker it crashed died; empty
    /// when an import failed it.
    FailureRecord record;

    /// The import whose build failed, or that closed a cycle back to the
    /// unit; invalid when the unit's own build failed.
    Fid import;
};

/// The PCM of a C++20 module unit, shared across all files that import the
/// module: its last build, and the failure of the last one that failed.
struct PCMEntry : Precompiled<ModuleBuildFailed> {
    /// The build's identity: "{module}-{hash}" over the unit's path and
    /// canonical flags, the build configuration, and the key of every module
    /// it imports (an unresolved import by its name) — so a command change
    /// anywhere below the module changes its key too.
    std::string key;

    /// The PCH the PCM was built over, which its importers read with it;
    /// empty for none.
    std::string pch;
};

/// A scan's module dependencies: the full durable edge set of a unit, a
/// node per module unit it imports and one unresolved import per name no
/// unit provides. A module unit's PCM is one node per unit in the task
/// graph, edged to the modules it imports; one attempt is one PCM build (or
/// the revalidation of the last one). The project owns the PCM policy:
/// cache key computation and hit checks, store commits, the modules' build
/// failures (PCMEntry::failure), and the cooperative response to advisory
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

/// The import scans of open buffers, and of units' disk text (see
/// Project::scan_imports): kept while the arguments and the project's disk
/// state (Project::disk_epoch) stay the same — and for a buffer, its directives.
/// A failed module's every importer waits on it again, and each wait would
/// preprocess it again.
struct ImportScans {
    llvm::DenseMap<Fid, ImportScan> buffers;

    llvm::DenseMap<Fid, ImportScan> disk;

    /// The preprocessor runs the scans took.
    std::uint64_t runs = 0;
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
