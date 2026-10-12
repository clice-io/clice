module;

#include "modules/prelude.h"

module clice:worker.probe;

import :support.signal;

namespace clice {

/// The builds a BuildProbe watches, named by the work they carry.
enum class BuildKind : std::uint8_t {
    Compile,
    PCH,
    PCM,
    Index,
};

/// The kinds' names in the protocol, by BuildKind.
constexpr inline std::array<llvm::StringLiteral, 4> build_kind_names = {
    "compile",
    "pch",
    "pcm",
    "index",
};

llvm::StringRef build_kind_name(BuildKind kind);

std::optional<BuildKind> parse_build_kind(llvm::StringRef name);

/// What the server's background loops do that a test waits for, as no
/// request of the test's starts it.
enum class LoopEvent : std::uint8_t {
    /// The database loop applied a change of a compilation database.
    DatabaseChange,
    /// The disk loop's looks found files changed.
    DiskChange,
    /// A checkpoint committed index shards.
    Checkpoint,
};

/// The events' names in the protocol, by LoopEvent.
constexpr inline std::array<llvm::StringLiteral, 3> loop_event_names = {
    "databaseChange",
    "diskChange",
    "checkpoint",
};

std::optional<LoopEvent> parse_loop_event(llvm::StringRef name);

/// Test instrumentation of the builds the worker pool dispatches, fed only
/// while test hooks are on (project.test_hooks, see WorkerPool::probe): how
/// many builds of each kind a worker ran per file, and holds that park a
/// build's reply between its arrival and its delivery. Parked, the build is
/// still in flight to the master — its round has not finished, the requests
/// waiting on it wait on — though its worker is free again: a test acts
/// while the master waits for a build, without making one slow. Watches
/// announce what the background loops did, which parks nothing.
class BuildProbe {
public:
    /// Builds a worker ran to their end — answered, or died running — per
    /// file, indexed by BuildKind. One the master withdrew (preempted,
    /// superseded) is not counted and passes every hold.
    llvm::StringMap<std::array<std::uint32_t, build_kind_names.size()>> builds;

    /// A worker ran a build of `kind` for `file` and its reply arrived:
    /// counts the build, then parks the reply while a hold takes it. A
    /// hold takes one reply, the first to arrive after it was placed.
    kota::task<> returned(BuildKind kind, llvm::StringRef file);

    /// Hold the next reply of `kind` for `file`, whenever it comes; returns
    /// the hold's id.
    std::uint64_t hold(BuildKind kind, std::string file);

    /// Gate the next request whose crash tag (worker::crash_tag) is `tag`:
    /// its work parks in the worker where it starts (see
    /// install_test_gates), the worker busy with it. Returns the gate's id,
    /// drawn with the holds'.
    std::uint64_t gate(std::string tag);

    /// A request of `tag` is about to go to the worker behind `peer`: the
    /// gate waiting for one, if any, goes ahead of it; returns its id.
    /// Called with every request the pool sends.
    std::optional<std::uint64_t> sending(llvm::StringRef tag,
                                         const std::shared_ptr<kota::ipc::BincodePeer>& peer);

    /// The request the gate went ahead of came back. A gate whose work it
    /// never reached — the request withdrawn first, or finding nothing to
    /// do — is dropped, in the worker too: left standing there, it would
    /// park the next request of its tag.
    void answered(std::uint64_t id);

    /// The worker parked the work of a gated request.
    void gate_parked(std::uint64_t id);

    /// Let the reply the hold parked, or the work the gate parked, go on;
    /// or drop a hold or gate nothing reached yet. False when none has the
    /// id.
    bool release(std::uint64_t id);

    /// Release every hold and gate: a stopping pool parks nothing.
    void release_all();

    /// Watch for the next `event`; on_held announces it under the returned
    /// id, drawn with the holds'.
    std::uint64_t watch(LoopEvent event);

    /// A background loop did `event`: every watch for it is announced and
    /// dropped.
    void happened(LoopEvent event);

    /// A hold parked a reply, a gate a request's work, or a watch saw its
    /// event; carries its id.
    Signal<std::uint64_t> on_held;

    struct Hold {
        std::uint64_t id;
        BuildKind kind;
        std::string file;

        /// A reply is parked on the hold.
        bool parked = false;

        /// Set by the release; the parked reply waits on it.
        std::shared_ptr<kota::event> released = std::make_shared<kota::event>();
    };

    struct Gate {
        std::uint64_t id;
        std::string tag;

        /// The gated request went to a worker, the one behind `worker`.
        bool sent = false;
        std::weak_ptr<kota::ipc::BincodePeer> worker;

        /// The worker parked the request's work.
        bool parked = false;
    };

    /// The holds placed and not released, in placement order.
    llvm::ArrayRef<Hold> holds() const {
        return placed;
    }

    /// The gates placed and not released, in placement order.
    llvm::ArrayRef<Gate> gates() const {
        return standing;
    }

private:
    struct Watch {
        std::uint64_t id;
        LoopEvent event;
    };

    std::uint64_t take_id();

    std::vector<Hold> placed;
    std::vector<Gate> standing;
    std::vector<Watch> watches;
    std::uint64_t next_id = 1;
};

}  // namespace clice
