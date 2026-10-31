module clice:project.pch;

import :command.table;
import :index.tu_index;
import :project.graph;
import :support.failure_record;
import :support.prelude;
import :syntax.header_context;
import :vfs.file_system;
import :vfs.ids;

namespace clice {

struct Project;

/// One acquisition of a preamble's PCH (a PCH and its pch.idx index,
/// committed as one pair) or of a precompiled header the build declares
/// (Build::pch_unit), built a whole file each: the key naming it plus the
/// inputs a build needs if the acquisition ends up dispatching one. A
/// preamble's PCH is built over the declared one its command reads.
///
/// One node per content-addressed key in the task graph, no edges; one
/// round is one revalidation or build. The project owns the shared side:
/// the pair-atomic store commit and cache metadata (a round runs to a real
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

/// What one acquisition observed.
enum class PCHOutcome : std::uint8_t {
    /// A fresh pair is registered under the key (revalidated or just
    /// built) — adopt it.
    Ready,
    Failed,
    /// The observed attempt ended without a result (advisory cancellation,
    /// shutdown): no adoption, but no failure result either.
    Preempted,
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

/// Join the key's round, spawning one when none is live. The spawning
/// acquisition is the dispatch owner: its inputs feed the build. One
/// acquisition observes exactly one attempt; retry policy stays with the
/// caller.
async::task<PCHOutcome> acquire_pch(Project& project, PCHRequest request);

/// The stash-only half of acquire_pch, for waits that must be recorded as
/// graph edges: intern the key, re-dirty a stale clean node, and install
/// the dispatch owner's inputs when the caller's join will spawn the round.
/// The caller then waits through RoundContext::depend on the returned node
/// — the only wait form a round may use.
NodeId prepare_pch(Project& project, PCHRequest request);

/// The request for clice's own build of the precompiled header the command
/// of `unit` writes (Build::pch_unit): the unit's whole file, under that
/// command. The editor's build skips function bodies, the index's keeps
/// them: they are two PCHs.
PCHRequest declared_pch_request(Project& project, const CommandRef& unit, bool skip_bodies);

/// Whether the PCH registered under `key` read `file`: a parse of that very
/// file cannot load it — clang refuses a PCH whose input the parse serves
/// from a buffer, and the guard the PCH defines would skip the file's text.
bool pch_reads(const Project& project, llvm::StringRef key, Fid file);

/// The crash of the key's last build, while the inputs it is known to have
/// read hold: a round's crash is recorded whatever became of the request
/// that spawned it (contract 12), and every consumer that finds it books it
/// against its own document instead of burning a worker of its own. One
/// document's records cannot contain it alone: the PCH is shared.
const FailureRecord* build_crash(Project& project, llvm::StringRef pch_key);

/// Forget the key's failed build, a crash or errors in the user's code, so
/// the next request builds it again: a consumer whose own record lets it
/// retry, or that was saved, retries it, and one moving to another key
/// releases it. Returns whether it had failed.
bool forget_build_failure(Project& project, llvm::StringRef pch_key);

/// A round for the key is in flight; its commit will republish over
/// concurrent store retractions.
bool pch_building(const Project& project, llvm::StringRef pch_key);

/// Discard a pair the frontend could not consume: remove both blobs from
/// the store and drop the idle cache entry, so the next acquisition misses
/// and rebuilds instead of trusting corrupt bytes for the life of the
/// store.
void discard_pch(Project& project, llvm::StringRef pch_key);

/// A parse reading the pair died or found it unreadable: discard it AND
/// count a failure of its consumption — unless the pair was gone from the
/// store by then: evicted under the parse, which says nothing of its bytes,
/// or discarded by a failure counted already. Each successful rebuild
/// clears the build's failure, so the consumption keeps a record of its
/// own; enough failures stop the key and acquisitions fail fast, so
/// consumers fall back to compiling without a PCH instead of rebuilding and
/// failing forever over failing storage.
void pch_consume_failed(Project& project, llvm::StringRef pch_key);

/// A parse read the key's pair and completed: its consumption failures were
/// transient — without this, two unrelated hiccups far apart would stop a
/// key that rebuilds fine in between.
void pch_consume_succeeded(Project& project, llvm::StringRef pch_key);

/// Open the pch.idx index of a cached PCH. The single consumption gate for
/// `.pch.idx` blobs: when the blob turns out unreadable, the on-disk pair
/// is discarded from the store as well — otherwise every later session
/// re-adopts the corrupt pair from the precompiled records and silently
/// degrades again. With the pair gone the next ensure_pch is a miss and
/// rebuilds both halves. Loads count against the loaded-state budget (see
/// enforce_loaded_budget).
std::shared_ptr<index::TUIndex> preamble_state(Project& project, llvm::StringRef pch_key);

/// Unload pch.idx indexes beyond the budget (two per open document, plus
/// 2), least recently used first. Without this every preamble key ever
/// touched keeps its blob mapped for the server's lifetime — tens of MB per
/// key on real projects, released by neither didClose nor store eviction.
/// Unloading only drops the entry's reference: consumers holding the
/// shared_ptr finish safely, and the next use reopens the blob from disk.
void enforce_loaded_budget(Project& project);

/// The buffer text the dispatch owners' requests hold (see
/// Project::pch_requests).
std::size_t stashed_pch_bytes(const Project& project);

/// One round of a PCH node, the graph's runner for NodeKind::PCH.
async::task<RoundOutcome> pch_round(Project& project, RoundContext& ctx, NodeId id);

}  // namespace clice
