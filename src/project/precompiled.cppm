module clice:project.precompiled;

import :project.graph;
import :project.project;
import :support.anomaly;
import :support.failure_record;
import :support.logging;
import :support.prelude;
import :vfs.file_table;
import :worker.pool;
import :worker.protocol;

namespace clice {

/// The precompiled records as the index database carries them (blob kind
/// PrecompiledRecords): the PCH and PCM validity metadata and the
/// self-containment trials, serialized from live state; empty on
/// serialization failure.
std::string serialize_precompiled_records(Project& project);

/// Restore the precompiled records a database held: the PCH and PCM entries
/// whose blobs the cache store still has, and the trials.
void load_precompiled_records(Project& project, llvm::StringRef bytes);

/// Length-prefix each part (so embedded NULs cannot create colliding
/// splits) and return the 32-hex xxh3_128bits cache key.
std::string cache_key(std::initializer_list<llvm::StringRef> parts);

/// The name in the store of a new build under `key`: the key and a nonce,
/// so a rebuild publishes beside the product it replaces, never over it.
std::string new_blob(llvm::StringRef key);

/// The record of a build that failed with `cause`: `death` is the identity
/// of the worker death it crashed, empty for errors in the user's code.
FailureRecord failed_build(llvm::StringRef death, std::string cause);

/// What a PCH or PCM build read: what the worker reports, and `before`,
/// what the modules it imports and the PCH it is built over were built from
/// as it started — one rebuilt meanwhile stales it.
DepsSnapshot read_inputs(Project& project,
                         const worker::PrecompiledBuildResult& built,
                         const DepsSnapshot& before);

/// Send a PCH or PCM build, `whom` naming what it builds in the log (a
/// file, a module), and keep what did not build: a build that
/// crashed its worker is recorded through `fail` on what it was known to
/// read (`known`), and one that failed on the user's code on what it read
/// (read_inputs over `before`); any other failure is clice's own breakage,
/// an anomaly. The interest class is read at dispatch time: a foreground
/// requester may have joined after the round started. The advisory token
/// rides into the pool, which cancels the request over IPC while this frame
/// keeps awaiting the real reply (contract 2). The reply of a build that
/// succeeded; else the round's outcome — Stale for a build the round
/// withdrew, which is no decision on it.
template <typename Params>
async::task<std::expected<worker::PrecompiledBuildResult, RoundOutcome>>
    send_precompiled(Project& project,
                     RoundContext& ctx,
                     const Params& params,
                     llvm::StringRef whom,
                     DepsSnapshot known,
                     const DepsSnapshot& before,
                     llvm::function_ref<void(FailureRecord, DepsSnapshot)> fail) {
    constexpr bool pch = std::same_as<Params, worker::BuildPCHParams>;
    constexpr llvm::StringRef what = pch ? "PCH" : "PCM";
    auto priority = ctx.foreground() ? worker::Priority::High : worker::Priority::Low;
    auto result = co_await deliver(
        *project.pool,
        false,
        [&] { return project.pool->send_stateless(params, priority, ctx.token()); },
        [&](const ipc::Error& error) {
            fail(failed_build(worker::death_of(error), error.message), std::move(known));
        });
    if(result.has_value() && result.value().success) {
        co_return std::move(result.value());
    }
    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("{} build preempted for {}, will retry", what, whom);
        co_return std::unexpected(RoundOutcome::Stale);
    }
    // A build is expected to fail on the user's errors, or for an operational
    // reason (memory-pressure preemption, crash/restart window).
    auto expected = result.has_value() ? result.value().has_user_errors
                                       : worker::is_operational_error(result.error());
    auto& message = result.has_value() ? result.value().error : result.error().message;
    if(expected) {
        LOG_WARN("{} build failed for {}: {}", what, whom, message);
        if(result.has_value()) {
            fail(failed_build({}, result.value().first_error),
                 read_inputs(project, result.value(), before));
        }
    } else if constexpr(pch) {
        LOG_ANOMALY(PCHBuildFail, "PCH build failed for {}: {}", whom, message);
    } else {
        LOG_ANOMALY(PCMBuildFail, "PCM build failed for {}: {}", whom, message);
    }
    co_return std::unexpected(RoundOutcome::Failed);
}

}  // namespace clice
