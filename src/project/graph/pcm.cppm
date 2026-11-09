module clice:project.graph.pcm;

import :command.database;
import :command.invocation;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :support.failure;
import :support.prelude;
import :vfs.file_table;
import :vfs.ids;

namespace clice {

/// A module unit's build that failed: on the user's code, on an import that
/// failed in turn, or by crashing a worker.
struct ModuleBuildFailed {
    /// The key of the build that failed (PCMEntry::key), on which a build
    /// is refused; empty when the failure decides nothing on the build's
    /// inputs: an import failed it and it never ran, no cache can store its
    /// PCM, or an import's PCM left the disk under it.
    std::string key;

    /// The build's first error, or how the worker it crashed died; empty
    /// when an import failed it.
    FailureRecord record;

    /// The PCM of the import whose build failed, or that closed a cycle back
    /// to the unit; none when the unit's own build failed.
    std::optional<NodeId> import;

    /// An import's PCM left the disk under the build: the failure says
    /// nothing of the module, and the next wait builds both again.
    bool import_lost = false;
};

/// The PCM of a C++20 module unit under one of its commands, shared across
/// all files that import the module and read that build: its last build,
/// and the failure of the last one that failed.
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

/// A scan's module dependencies: every edge a unit declares, a
/// node per module unit it imports and one unresolved import per name no
/// unit provides. A module unit's PCM is one node per unit and command it
/// builds under in the task graph (module_node), edged to the PCMs it
/// imports; one attempt is one PCM build (or the revalidation of the last
/// one). The project owns the PCM policy:
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

    /// What the scan read: what the import list stands on.
    Inputs inputs;

    /// The PCMs of `declared`: what a consumer waits on.
    llvm::SmallVector<NodeId> resolved() const;

    /// A parse of the unit meets a module declaration, which no declaration
    /// may precede, or a PCM, which names the precompiled header it was
    /// built over to its importers: over the precompiled header its command
    /// declares, it reads the one every PCM is built over.
    bool modular() const;
};

/// The imports of a parse of an open buffer, their modules built
/// (Project::build_imports).
struct BuiltImports {
    Imports imports;

    /// The crashes of the imports whose build crashed a worker, not built
    /// again (BuildOptions::retry_crashed).
    llvm::SmallVector<FailureRecord> refused;

    /// The imports that did not build.
    llvm::SmallVector<NodeId> failed;
};

/// A unit's last precise import scan (Project::scan_imports) and what it
/// ran against beside the files it read: a failed module's every importer
/// waits on it again, and each wait would preprocess it again.
struct ImportScan {
    /// The directives hash of a buffer's text.
    std::uint64_t directives = 0;

    /// The preamble bound of the scanned text: the import offsets are
    /// positions, which an edit moving the preamble without changing a
    /// directive moves too.
    std::uint32_t bound = 0;

    std::uint64_t arguments = 0;

    std::vector<std::string> imports;

    std::vector<std::uint32_t> offsets;

    bool module_unit = false;
};

/// The graph identity of an import that resolves to nothing: a node that
/// never runs an attempt and exists only to be edged at. When the name's
/// first provider appears, the change's cascade invalidates it
/// (Project::files_changed, update_members) and reaches every consumer whose
/// scan declared the edge — no side bookkeeping of who failed against the
/// name. The high bit keeps the key
/// space disjoint from path_ids.
inline NodeId unresolved_import(llvm::StringRef name) {
    return {NodeKind::PCM, (1ull << 63) | (llvm::xxh3_64bits(name) >> 1)};
}

inline bool is_unresolved_import(NodeId id) {
    return id.kind == NodeKind::PCM && (id.key >> 63) != 0;
}

/// The graph node of a module unit's PCM: its build under its own command,
/// or, for a nonzero `context` (ModuleContexts), under another of its
/// database entries — the contexts the unit compiles in.
inline NodeId module_node(Fid module, std::uint32_t context = 0) {
    assert(context >> 31 == 0);
    return {NodeKind::PCM, module.raw | (std::uint64_t(context) << 32)};
}

/// The context a PCM node builds its unit in (module_node).
inline std::uint32_t module_context(NodeId node) {
    return static_cast<std::uint32_t>(node.key >> 32);
}

/// The contexts a module unit's PCM builds in: the first under its own
/// command, then one under each other entry whose command differs, by an id
/// from the entry's command hash, so that a context keeps naming the same
/// command across database reloads. Each with what decides which importers
/// can read that build (module_compatibility).
struct ModuleContexts {
    struct Context {
        std::uint32_t id = 0;

        /// The databases listing the unit under this command.
        llvm::SmallVector<SourceID, 1> databases;

        ModuleCompatibility compatibility;
    };

    llvm::SmallVector<Context, 2> listed;

    /// Project::commands_epoch when `listed` was taken.
    std::uint64_t epoch = 0;

    /// The contexts importers were given: the unit's PCM nodes besides its
    /// own (Project::module_nodes).
    llvm::SmallVector<std::uint32_t, 1> picked;
};

/// The PCM nodes a node of the graph waits on: its imports that resolve.
inline llvm::SmallVector<NodeId> imported_modules(const TaskGraph& graph, NodeId importer) {
    llvm::SmallVector<NodeId> modules;
    for(auto dep: graph.dependencies(importer)) {
        if(dep.kind == NodeKind::PCM && !is_unresolved_import(dep)) {
            modules.push_back(dep);
        }
    }
    return modules;
}

/// How many times a compile waits on its imports for all of them to be
/// present at once: a rebuild can itself evict another module under budget
/// pressure. Past that a module build fails unrecorded, and a parse fails
/// visibly on the missing file.
constexpr inline int import_passes = 3;

}  // namespace clice
