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

/// Length-prefix each part (so embedded NULs cannot create colliding
/// splits) and return the 32-hex xxh3_128bits cache key.
inline std::string cache_key(std::initializer_list<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

/// The record of a build that failed with `cause`: `death` is the identity
/// of the worker death it crashed, empty for errors in the user's code.
inline FailureRecord failed_build(llvm::StringRef death, std::string cause) {
    FailureRecord record;
    record.fail({}, death, std::move(cause), FailureRecord::Clock::now());
    return record;
}

/// What a PCH or PCM build read: what the worker reports, and `before`,
/// what the modules it imports and the PCH it is built over were built from
/// as it started — one rebuilt meanwhile stales it.
inline DepsSnapshot read_inputs(Project& project,
                                const worker::PrecompiledBuildResult& built,
                                const DepsSnapshot& before) {
    auto deps = capture_deps_snapshot(project.file_table, built.deps, built.build_at);
    merge_deps(deps, {&before});
    return deps;
}

/// Send a PCH or PCM build, `what` naming it in the log ("PCH" for `whom`,
/// a file; "PCM" for a module), and keep what did not build: a build that
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
                     llvm::StringRef what,
                     llvm::StringRef whom,
                     DepsSnapshot known,
                     const DepsSnapshot& before,
                     llvm::function_ref<void(FailureRecord, DepsSnapshot)> fail) {
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
    } else if constexpr(std::same_as<Params, worker::BuildPCHParams>) {
        LOG_ANOMALY(PCHBuildFail, "PCH build failed for {}: {}", whom, message);
    } else {
        LOG_ANOMALY(PCMBuildFail, "PCM build failed for {}: {}", whom, message);
    }
    co_return std::unexpected(RoundOutcome::Failed);
}

}  // namespace clice
