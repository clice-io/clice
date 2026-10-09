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

/// The kinds' names on the wire, by BuildKind.
constexpr inline std::array<llvm::StringLiteral, 4> build_kind_names = {
    "compile",
    "pch",
    "pcm",
    "index",
};

llvm::StringRef build_kind_name(BuildKind kind);

std::optional<BuildKind> parse_build_kind(llvm::StringRef name);

/// Test instrumentation of the builds the worker pool dispatches, fed only
/// while test hooks are on (project.test_hooks, see WorkerPool::probe): how
/// many builds of each kind a worker ran per file, and holds that park a
/// build's reply between its arrival and its delivery. Parked, the build is
/// still in flight to the master — its round has not landed — so a test
/// acts inside the window a slow build would open, without making one slow.
class BuildProbe {
public:
    /// Builds a worker ran, or died running, per file, indexed by
    /// BuildKind.
    llvm::StringMap<std::array<std::uint32_t, build_kind_names.size()>> builds;

    /// A worker ran a build of `kind` for `file` and its reply arrived:
    /// counts the build, then parks the reply while a hold claims it. A
    /// hold claims one reply, the first to arrive after it was placed.
    kota::task<> returned(BuildKind kind, llvm::StringRef file);

    /// Hold the next reply of `kind` for `file`; returns the hold's id.
    std::uint64_t hold(BuildKind kind, std::string file);

    /// Let the reply the hold parked go on, or drop a hold no reply reached
    /// yet. False when no hold has the id.
    bool release(std::uint64_t id);

    /// Release every hold: a stopping pool parks no reply.
    void release_all();

    /// A hold parked a reply; carries the hold's id.
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

    /// The holds placed and not released, in placement order.
    llvm::ArrayRef<Hold> holds() const {
        return placed;
    }

private:
    std::vector<Hold> placed;
    std::uint64_t next_id = 1;
};

}  // namespace clice
