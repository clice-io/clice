module clice:server.test_hooks;

import :support.prelude;

namespace clice::ext {

/// clice/internal/poll — TEST-ONLY, not a stable API. Runs a look at the
/// configuration files, then one look at the disk (the databases, or the
/// files and the default-command sources) and responds only once the
/// changes are applied, so integration tests can disable the polling loops
/// and get "change disk → poll → assert" determinism with zero sleeps.
/// Absent from capabilities and user docs.
struct PollParams {
    /// Which loop to tick: "cdb" or "workspace".
    std::string loop;

    /// CDB loop only; defaults to true. A forced tick reloads unconditionally
    /// — no content gate, no stability debounce — so one request applies a
    /// change deterministically. `false` runs the production tick, for
    /// tests that pin the content gate itself.
    std::optional<bool> force;
};

struct PollResult {
    /// How many changes the look applied: changed files, database reloads
    /// that changed a build, projects whose default sources changed.
    std::uint32_t events = 0;
};

/// Test hook (clice/internal/logFlood): emit `count` info-level log lines
/// of roughly `size` bytes each, tagged stderr-flood with a running index.
/// Gives backpressure tests a deterministic volume source — feature log
/// lines change shape over time and must not be load-bearing for tests.
/// Absent from capabilities and user docs.
struct LogFloodParams {
    std::uint32_t count = 0;
    std::uint32_t size = 0;
};

struct LogFloodResult {
    std::uint32_t emitted = 0;
};

/// clice/internal/stats — TEST-ONLY, not a stable API. Ownership gauges
/// for memory-lifecycle regression tests: instead of brittle RSS
/// assertions, each leak class is pinned by a deterministic counter
/// (consumed by the tests/lifecycle/memory cases); and
/// the counts of freshness checks, which pin what a request looks at.
/// Absent from capabilities and user docs.
struct StatsParams {};

struct FileBuilds {
    std::string uri;
    std::uint32_t compile = 0;
    std::uint32_t pch = 0;
    std::uint32_t pcm = 0;
    std::uint32_t index = 0;
};

struct WorkerPids {
    std::vector<std::uint32_t> stateful;
    std::vector<std::uint32_t> stateless;
};

struct StatsResult {
    /// PCH entries whose pch.idx index is currently open, and
    /// their mapped bytes. Steady state after closing documents: bounded
    /// by the loaded-index budget, not by every key ever touched.
    std::uint32_t pch_loaded_indexes = 0;
    std::uint64_t pch_index_bytes = 0;

    /// Shard blobs awaiting persistence (the indexer's dirty set — zero
    /// after a finished save), and the total mapped bytes of every loaded
    /// shard blob.
    std::uint32_t index_inmemory_shards = 0;
    std::uint64_t index_shard_content_bytes = 0;

    /// Shards the last index save actually wrote (the true dirty set).
    std::uint32_t last_save_shards = 0;

    /// In-flight tmp blobs of this instance's cache store. Zero once
    /// builds finish — every pending write either committed or cleaned
    /// itself up.
    std::uint32_t pending_tmp_files = 0;

    /// Trend gauges.
    std::uint32_t pch_cache_entries = 0;
    std::uint32_t header_contexts = 0;
    /// Of those, the ones whose includer context was synthesized.
    std::uint32_t synthesized_contexts = 0;
    std::uint32_t sessions = 0;

    /// Freshness checks of files answered by a look at the disk, and from
    /// a look not yet due (see vfs::DiskState::Checks).
    std::uint64_t checks_looked = 0;
    std::uint64_t checks_trusted = 0;

    /// Preprocessor passes that looked for a unit's imports.
    std::uint64_t import_scans = 0;

    /// Whether no project has background indexing queued or running: what
    /// the index serves is all it will serve until the next change.
    bool index_idle = true;

    /// The builds each file went through, while test hooks are on (see
    /// TestHooks); empty otherwise.
    std::vector<FileBuilds> builds;

    /// The process ids of the workers in service, by kind.
    WorkerPids workers;
};

/// clice/internal/sync — TEST-ONLY, present while project.test_hooks is on.
/// Answers once the server has no work left: no other request of the
/// editor unanswered, no compile, PCH or PCM build and no index pass in
/// flight or queued, a pass's save included, and no removed folder still
/// shutting down. The metadata save that follows a build is not waited
/// for; a test needing what is on disk stops the server. Absent from
/// capabilities and user docs.
struct SyncParams {
    /// How long after the request to answer with the work still pending,
    /// four minutes by default; a poll runs to its end first.
    std::optional<std::uint32_t> deadline_ms;

    /// Tick the workspace loop first (see PollParams), so changes on disk
    /// are part of the work waited for.
    std::optional<bool> poll;
};

struct SyncResult {
    /// Files whose latest index attempt failed for good.
    std::vector<std::string> failed;

    /// Index state remains that no save committed.
    bool unsaved = false;

    /// What still ran at the deadline, one line each; empty once idle.
    std::vector<std::string> pending;
};

/// clice/internal/hold — TEST-ONLY, present while project.test_hooks is on.
/// Parks the next reply of a build of `kind` ("compile", "pch", "pcm" or
/// "index") for the file between its arrival and its delivery: to the
/// server the build is still in flight. clice/internal/held announces the
/// parked reply; clice/internal/release lets it go on (see TestHooks).
struct HoldParams {
    std::string kind;
    std::string uri;
};

struct HoldResult {
    std::uint64_t id = 0;
};

/// clice/internal/gate — TEST-ONLY, present while project.test_hooks is on.
/// Parks the work of the next request named `request` for the file in its
/// worker, where the work starts: the worker is busy with it (see
/// TestHooks::gate). `request` is the worker request's name as its crash
/// tag spells it ("compile", "completion", "signatureHelp", "format",
/// "buildPch", "compileUnit", "hover"). The reply is a hold id:
/// clice/internal/held announces the park, clice/internal/release lets the
/// work go on.
struct GateParams {
    std::string request;
    std::string uri;
};

/// clice/internal/watch — TEST-ONLY, present while project.test_hooks is on.
/// Watches for the next `event` of the server's background loops after it
/// is placed: "databaseChange", the database loop applied a changed
/// compilation database; "diskChange", the disk loop's looks found files
/// changed; "checkpoint", a checkpoint committed index shards (see
/// LoopEvent). The reply is a hold id: clice/internal/held announces the
/// event.
struct WatchParams {
    std::string event;
};

struct ReleaseParams {
    std::uint64_t id = 0;
};

struct ReleaseResult {};

/// Notification: the hold parked a reply, the gate a request's work, or the
/// watch saw its event.
struct HeldParams {
    std::uint64_t id = 0;
};

}  // namespace clice::ext
