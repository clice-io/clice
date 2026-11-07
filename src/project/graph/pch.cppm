module clice:project.graph.pch;

import :index.tu_index;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :support.failure_record;
import :support.prelude;
import :syntax.header_context;
import :vfs.file_system;
import :vfs.ids;
import :worker.protocol;

namespace clice {

/// Open a PCH's `.pch.idx` index (memory-mapped). Returns nullptr when
/// the file is unreadable, structurally invalid, of a different format
/// version, or any embedded shard blob fails verification — callers treat
/// all of these as a PCH cache miss.
std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path);

/// A PCH the cache holds, in Project::pchs under its key (hex of
/// xxh3_128bits over preamble text + directories + canonical flags), so
/// files with identical preambles share one PCH; a preamble's product is
/// the pair of the PCH and its pch.idx index, committed and evicted
/// together.
///
/// Everything derived from a preamble's PCH build beyond validity metadata
/// — the preamble's symbol index, document links, inactive regions, the
/// open conditional stack — lives in the pch.idx index. A precompiled
/// header the build declares pairs with none: what its build indexes is
/// the writing unit's, in the project index.
struct PCHEntry : Precompiled<FailureRecord> {
    std::uint32_t bound = 0;

    /// A precompiled header the build declares, built whole
    /// (Project::declared_pch_request).
    bool whole_file = false;

    /// The key of the precompiled header the build declares that this
    /// preamble's PCH is built over and chains onto; empty for none.
    std::string base;

    /// The blob of the base's build it chains onto: the PCH names that very
    /// file, so a rebuilt base, under another name, leaves it nothing to
    /// load.
    std::string base_blob;

    /// Path of the paired pch.idx index.
    std::string index_path;

    /// The opened blob while someone holds it — a document's compile, the
    /// parse its build was for — so it stays mapped only while used; a
    /// holder keeps it across an await, whatever becomes of the entry.
    std::weak_ptr<index::TUIndex> index;

    /// The blob the build opened and verified on the thread pool, until the
    /// parse waiting for it takes it (BuiltPCH::index); none when no parse
    /// waited any more.
    std::shared_ptr<index::TUIndex> built_index;

    /// The parses that died reading the key's pairs or found them unreadable
    /// (Project::pch_consume_failed); none since one read them.
    std::optional<FailureRecord> consumes;

    /// The blob while held, else opened again (memory-mapped, no
    /// deserialization). Returns nullptr when the blob is missing or
    /// unreadable — consumers degrade (no overlay, no preamble links) and the
    /// next PCH attempt treats the incomplete pair as a cache miss.
    std::shared_ptr<index::TUIndex> load_index();
};

/// One acquisition of a preamble's PCH (a PCH and its pch.idx index,
/// committed as one pair) or of a precompiled header the build declares
/// (Project::pch_writer), built a whole file each: the key naming it plus the
/// inputs a build needs if the acquisition ends up dispatching one. A
/// preamble's PCH is built over the declared one its command reads.
///
/// One node per content-addressed key in the task graph, no edges; one
/// attempt is one revalidation or build. The project owns the shared side:
/// the pair-atomic store commit and cache metadata (an attempt runs to a real
/// reply, so no cancellation can split blob from metadata), the failures
/// of the keys' builds and consumption (PCHEntry). The adoption
/// side stays with the server: which document points at the key and the
/// documents' failure records — every consumer reads a failed acquisition's
/// crash from pch_build_crash and books it for itself.
struct PCHRequest {
    std::string pch_key;

    /// What the worker builds from: the file, its command, the preamble's
    /// text and bound or the whole file, the modules' PCMs. The paths the
    /// build writes to and the PCH it builds over are the build's to fill.
    worker::BuildPCHParams params;

    /// The built modules the preamble imports, whose inputs the PCH depends
    /// on.
    llvm::SmallVector<NodeId> modules;

    /// The key of the precompiled header the build declares for the command
    /// (declared_pch_request), built already: the preamble's PCH is built
    /// over it, and what it read is the preamble's input too.
    std::string base;

    /// The unit writing the precompiled header the build declares, when the
    /// build compiles it under the unit's own command: the build is the
    /// unit's compile, and its index the unit's, recorded under the identity
    /// of that command as the request rendered it. None for a preamble's.
    struct Writer {
        Fid unit;
        std::uint64_t command = 0;
    };

    std::optional<Writer> writer;
};

/// The PCH of a document's preamble (Project::preamble_pch): the request
/// acquiring it; none when the preamble holds nothing to precompile.
struct PreamblePCH {
    std::optional<PCHRequest> request;

    /// The preamble ends before the text's own, at a completion's point:
    /// what the request loads is its own, not the document's.
    bool partial = false;
};

/// The PCHs a parse of an open buffer loads, built (Project::build_pch).
struct BuiltPCH {
    /// The key of the precompiled header the build declares for the
    /// command, built: the preamble's PCH is built over it.
    std::optional<std::string> base;

    /// The key of the preamble's PCH; none when the preamble holds nothing
    /// to precompile.
    std::optional<std::string> preamble;

    /// The preamble ends before the text's own (PreamblePCH::partial).
    bool partial = false;

    /// The crash of the preamble's PCH build, not built again
    /// (BuildOptions::retry_crashed).
    std::optional<FailureRecord> refused;

    /// How the wait for the preamble's PCH ended; Failed when refused.
    Result result = Result::Failed;

    /// The key of the PCH the parse loads: the preamble's, else the
    /// declared one; none for neither.
    std::optional<std::string> loaded;

    /// The path and bound of `loaded`, as a worker's compile takes them.
    std::pair<std::string, std::uint32_t> pch;

    /// The preamble's index its build opened and verified, for the parse
    /// this build ran for; null for a PCH built before, which the parse
    /// opens again (Project::preamble_index).
    std::shared_ptr<index::TUIndex> index;
};

/// When reading a key's pairs stops it from building: two failures in a
/// row within the cooldown. A stop must never be final: what fails may live
/// in content the key cannot see (a header the preamble includes), so
/// editing it cannot unlock the key. After the cooldown the key builds
/// again, and stops again only after `limit` more failures — bounded burn,
/// never a permanent decision, like slot revival.
constexpr inline FailurePolicy pch_consume_policy{
    .limit = 2,
    .cooldown = chrono::minutes(5),
};

/// The graph node of the PCH registered under `pch_key`.
inline NodeId pch_node(llvm::StringRef pch_key) {
    return {NodeKind::PCH, llvm::xxh3_64bits(pch_key)};
}

}  // namespace clice
