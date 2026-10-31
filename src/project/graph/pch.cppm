module clice:project.pch;

import :command.table;
import :index.tu_index;
import :project.task_graph;
import :support.failure_record;
import :support.prelude;
import :syntax.header_context;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.ids;

namespace clice {

/// Open a PCH's `.pch.idx` index (memory-mapped). Returns nullptr when
/// the file is unreadable, structurally invalid, of a different format
/// version, or any embedded shard blob fails verification — callers treat
/// all of these as a PCH cache miss.
std::shared_ptr<index::TUIndex> load_pch_index(llvm::StringRef path);

/// A PCH the cache holds, in Project::pch_cache under its key (hex of
/// xxh3_128bits over preamble text + directories + canonical flags), so
/// files with identical preambles share one PCH.
///
/// Everything derived from the PCH build beyond validity metadata — the
/// preamble's symbol index, document links, inactive regions, the open
/// conditional stack — lives in the paired pch.idx index (the store's
/// `.pch.idx` aux file), committed and evicted together with the PCH.
struct PCHBuild {
    /// The pair's name in the store: the key and a nonce of the build that
    /// produced it. A rebuild publishes under a new name, never over the
    /// old pair, which workers may still have mapped — Windows refuses to
    /// replace a mapped file.
    std::string blob;

    /// The pair the latest build replaced, kept for one more build: a
    /// consumer that took its path just before the replacement may not
    /// have opened it yet.
    std::string previous;

    std::string path;
    std::uint32_t bound = 0;
    Inputs inputs;

    /// The key of the precompiled header the build declares that this
    /// preamble's PCH is built over and chains onto; empty for none.
    std::string base;

    /// The blob of the base's build it chains onto: the PCH names that very
    /// file, so a rebuilt base, under another name, leaves it nothing to
    /// load.
    std::string base_blob;

    /// Path of the paired pch.idx index.
    std::string index_path;

    /// Lazily opened blob; shared so a consumer holding it across an await
    /// survives concurrent entry replacement or eviction.
    std::shared_ptr<index::TUIndex> index;

    /// Open the blob on first use (memory-mapped, no deserialization).
    /// Returns nullptr when the blob is missing or unreadable — consumers
    /// degrade (no overlay, no preamble links) and the next PCH attempt
    /// treats the incomplete pair as a cache miss.
    const std::shared_ptr<index::TUIndex>& load_index();
};

/// One acquisition of a preamble's PCH (a PCH and its pch.idx index,
/// committed as one pair) or of a precompiled header the build declares
/// (Build::pch_unit), built a whole file each: the key naming it plus the
/// inputs a build needs if the acquisition ends up dispatching one. A
/// preamble's PCH is built over the declared one its command reads.
///
/// One node per content-addressed key in the task graph, no edges; one
/// attempt is one revalidation or build. The project owns the shared side:
/// the pair-atomic store commit and cache metadata (an attempt runs to a real
/// reply, so no cancellation can split blob from metadata), the failures
/// of the keys' builds and consumption (Project::failures). The adoption
/// side stays with the server: which document points at the key and the
/// documents' failure records — every consumer reads a failed acquisition's
/// crash from build_crash and books it for itself.
struct PCHRequest {
    std::string pch_key;

    std::string file;

    std::string directory;

    std::vector<std::string> arguments;

    /// See worker::BuildPCHParams::hidden.
    vfs::HiddenFiles hidden;

    /// The requesting document's buffer text; the preamble is its prefix
    /// and the build sends the whole buffer.
    std::string content;

    std::uint32_t preamble_bound = 0;

    /// See worker::BuildPCHParams::synthesized.
    SynthesizedFiles synthesized;

    /// See worker::BuildPCHParams::pcms.
    std::unordered_map<std::string, std::string> pcms;

    /// The built modules the preamble imports, whose inputs the PCH depends
    /// on.
    llvm::SmallVector<Fid> modules;

    /// The key of the precompiled header the build declares for the command
    /// (declared_pch_request), built already: the preamble's PCH is built
    /// over it, and what it read is the preamble's input too.
    std::string base;

    /// See worker::BuildPCHParams::whole_file.
    bool whole_file = false;

    /// See worker::BuildPCHParams::skip_bodies.
    bool skip_bodies = false;
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
