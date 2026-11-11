module clice;

import :project.command;
import :project.graph.pcm;
import :project.graph.precompiled;
import :project.graph.task_graph;
import :project.graph.unit;
import :project.project;
import :support.failure;
import :support.logging;
import :support.text;
import :vfs.cache_store;
import :vfs.file_table;
import :worker.pool;
import :worker.protocol;

namespace clice {

std::string cache_key(llvm::ArrayRef<llvm::StringRef> parts) {
    std::string input;
    for(auto part: parts) {
        input += std::format("{}:", part.size());
        input += part;
    }
    auto hash = llvm::xxh3_128bits(llvm::arrayRefFromStringRef(input));
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

std::string new_blob(llvm::StringRef key) {
    std::random_device random;
    return std::format("{}-{:08x}{:08x}", key, random(), random());
}

FailureRecord failed_build(llvm::StringRef death, std::string cause) {
    FailureRecord record;
    record.fail({}, death, std::move(cause), FailureRecord::Clock::now());
    return record;
}

Inputs Project::read_inputs(const worker::PrecompiledBuildResult& built, const Inputs& before) {
    auto inputs = intern_inputs(file_table, built.deps, built.started_at);
    merge_inputs(inputs, {&before});
    return inputs;
}

void Project::merge_build_index(Fid unit,
                                IndexTransfer& transfer,
                                const worker::PrecompiledBuildResult& built,
                                const Inputs& before,
                                std::uint64_t command) {
    // One the build stopped compiling while it ran left the index with its
    // entries.
    if(!is_unit(unit)) {
        return;
    }
    auto path = file_table.resolve(unit);
    auto tu_index = transfer.receive(built.tu_index_data, built.index_in_file);
    if(!tu_index) {
        LOG_WARN("The index the build of {} sent is lost: {}", path, tu_index.error());
        return;
    }
    // A build measures no peak: the unit's last compile's stands.
    auto manifest = index.manifest(unit);
    if(!merge_index(unit, *tu_index, before, command, manifest ? manifest->peak : 0)) {
        LOG_WARN("The index the build of {} sent did not merge", path);
    }
}

/// Whether a file a build read holds other bytes by now: a save rewrites
/// a file in place, and a build reading it then may read it half written,
/// a change no one sees when the save leaves the bytes as they were.
static bool rewritten_since(FileTable& files, const Inputs& read) {
    return ranges::any_of(read, [&](const Input& input) {
        return !input.absent && input.version.valid() &&
               files.check_version(input.version) == vfs::DiskState::CheckResult::Stale;
    });
}

template <typename Params>
static async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    send_build(Project& project,
               Attempt& attempt,
               const Params& params,
               llvm::StringRef label,
               llvm::StringRef why,
               Inputs known,
               const Inputs& before,
               llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    constexpr bool pch = std::same_as<Params, worker::BuildPCHParams>;
    constexpr llvm::StringRef what = pch ? "PCH" : "PCM";
    LOG_INFO("Building the {} of {}: {}", what, label, why);
    Timer timer;
    auto result = co_await deliver(
        *project.pool,
        false,
        [&] {
            return project.pool->send_stateless(
                params,
                {.priority = attempt.foreground() ? worker::Priority::High : worker::Priority::Low,
                 .raised = &attempt.on_foreground(),
                 .cancel = attempt.token()});
        },
        [&](const ipc::Error& error) {
            fail(failed_build(worker::death_of(error), error.message), std::move(known));
        });
    if(result.has_value() && result.value().success) {
        LOG_INFO("Built the {} of {} in {} ms", what, label, timer.ms());
        co_return std::move(result.value());
    }
    if(!result.has_value() && result.error().code == worker::dispatch_errc::cancelled) {
        LOG_INFO("The {} build of {} was stopped for more urgent work; it runs again later",
                 what,
                 label);
        co_return std::unexpected(Result::Stale);
    }
    // A build is expected to fail on the user's errors, or for an operational
    // reason (memory-pressure preemption, crash/restart window).
    auto expected = result.has_value() ? result.value().has_user_errors
                                       : worker::is_operational_error(result.error());
    auto& message = result.has_value() ? result.value().error : result.error().message;
    Inputs read;
    if(result.has_value() && result.value().has_user_errors) {
        read = project.read_inputs(result.value(), before);
        if(rewritten_since(project.file_table, read)) {
            LOG_INFO(
                "The {} build of {} failed on files rewritten since it read them; it runs "
                "again",
                what,
                label);
            co_return std::unexpected(Result::Stale);
        }
    }
    if(expected) {
        std::string shown = message;
        if(result.has_value() && result.value().has_user_errors && result.value().error_count > 1) {
            auto more = result.value().error_count - 1;
            shown = std::format("{} (and {}, all in the worker's log)",
                                result.value().first_error,
                                counted(more, "more error", "more errors"));
        }
        LOG_WARN("The {} build of {} failed: {}", what, label, shown);
        if(result.has_value()) {
            fail(failed_build({}, result.value().first_error), std::move(read));
        }
    } else if constexpr(pch) {
        LOG_ANOMALY(PCHBuildFail, "PCH build failed for {}: {}", label, message);
    } else {
        LOG_ANOMALY(PCMBuildFail, "PCM build failed for {}: {}", label, message);
    }
    co_return std::unexpected(Result::Failed);
}

async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    Project::send_precompiled(Attempt& attempt,
                              const worker::BuildPCHParams& params,
                              llvm::StringRef label,
                              llvm::StringRef why,
                              Inputs known,
                              const Inputs& before,
                              llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    return send_build(*this, attempt, params, label, why, std::move(known), before, fail);
}

async::task<std::expected<worker::PrecompiledBuildResult, Result>>
    Project::send_precompiled(Attempt& attempt,
                              const worker::BuildPCMParams& params,
                              llvm::StringRef label,
                              llvm::StringRef why,
                              Inputs known,
                              const Inputs& before,
                              llvm::function_ref<void(FailureRecord, Inputs)> fail) {
    return send_build(*this, attempt, params, label, why, std::move(known), before, fail);
}

}  // namespace clice
