#include "driver/query_support.h"

#include "config/config.h"
#include "index/writer_lock.h"
#include "project/configuration.h"
#include "sched/batch.h"
#include "server/control_client.h"

namespace clice::driver {

/// Bring the index up to date with the disk before a --fresh answer:
/// through the serving writer when a server holds the lock, else by a
/// batch run of this process. Either sweeps the build under the hash
/// gate, so only units whose inputs changed are recompiled — and an
/// absent index gets built from nothing. Returns the units that failed
/// to index.
std::expected<std::vector<std::string>, std::string> refresh(const Spelling& workspace,
                                                             llvm::StringRef configuration,
                                                             const char* self_path) {
    auto config = Config::load_from_workspace(CanonicalPath(workspace));
    if(!check_requested_configuration(config, configuration)) {
        return std::unexpected(
            std::format("unknown configuration '{}'", std::string_view(configuration)));
    }
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::probe_writer(cache_dir);
    switch(writer.state) {
        case index::WriterProbe::State::Server: {
            auto result = control::request_index(writer.endpoint,
                                                 resolve_configuration(config, configuration));
            if(!result) {
                return std::unexpected(result.error());
            }
            return std::move(result->failed);
        }
        case index::WriterProbe::State::Held: {
            return std::unexpected(index::held_writer_message(writer, cache_dir));
        }
        case index::WriterProbe::State::Free: break;
    }
    auto report_progress = [](const BatchProgress& progress) {
        driver::println(stderr,
                        "indexing {}/{} units, {} failed",
                        progress.completed,
                        progress.total,
                        progress.failed);
    };
    auto result = run_batch_index({
        .root = workspace,
        .configuration = configuration.str(),
        .self_path = self_path,
        .on_progress = report_progress,
    });
    if(!result.completed) {
        return std::unexpected(result.interrupted ? "indexing interrupted"
                                                  : "indexing failed; see the log");
    }
    if(result.unsaved) {
        return std::unexpected("part of the index could not be persisted; see the log");
    }
    return std::move(result.failed);
}

}  // namespace clice::driver
