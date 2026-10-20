module;

#include "modules/prelude.h"

module clice:command.toolchain;

import :command.command;

namespace clice {

/// Resolves driver-level configs into full cc1-level configurations by
/// probing the compiler driver, in two layers:
///
///   - Probe layer: keyed by (driver, input language, the args that may
///     change the driver's job, and — only for cwd-sensitive configs — the
///     directory). One probe per unique key finds the cc1 job the driver
///     runs; its frontend (see Frontend) and failures (with a retry
///     cooldown) are cached. User-content flags (-I, -D, ...) never affect
///     the probe.
///
///   - Synthesis layer: keyed by (config, input). The probe's frontend with
///     the config's own user content re-attached.
class Toolchain {
public:
    using ResolvedID = std::uint32_t;

    /// A synthesized full compile configuration, self-contained: rendering
    /// it needs no trip back to the source config.
    struct Resolved {
        /// The compiler binary the probe resolved.
        const char* driver = nullptr;

        /// Frontend-level args (cc1): render as `driver -cc1 <args>`.
        llvm::ArrayRef<Arg> args;
    };

    /// `failed_retry` bounds the negative cache: a failed key is retried
    /// once the cooldown passes (cf. BlameBudget), so a transient driver
    /// failure — an upgrade replacing the binary mid-stat, a full tmpfs —
    /// cannot poison the key for the rest of the session.
    explicit Toolchain(CompilationDatabase& db,
                       std::chrono::steady_clock::duration failed_retry = std::chrono::seconds(30));
    ~Toolchain();

    /// Resolve a config for one input language. Blocks on a driver spawn on
    /// probe miss; hits are lookups.
    std::expected<ResolvedID, std::string> resolve(ConfigID id, InputKind input);

    const Resolved& resolved(ResolvedID id) const {
        return resolved_configs[id];
    }

    /// Pre-probe in parallel, deduplicated by probe key, running as many
    /// drivers at once as the process has CPUs. Blocks until all unique
    /// probes complete.
    void warm(llvm::ArrayRef<std::pair<ConfigID, InputKind>> pairs);

    bool has_cache() const {
        return !probes.empty();
    }

    static CompilerFamily driver_family(llvm::StringRef driver);

    std::string probe_key_for(ConfigID id, InputKind input) {
        return probe_key(id, input).key;
    }

    void set_failed_retry(std::chrono::steady_clock::duration retry) {
        failed_retry = retry;
    }

    std::size_t probe_count() const {
        return probes.size();
    }

    std::size_t failed_count() const {
        return failed.size();
    }

private:
    struct ProbeKey {
        std::string key;
        bool cwd_sensitive = false;
    };

    ProbeKey probe_key(ConfigID id, InputKind input);

    /// The probe argv: driver (+ subcommand) + non-user-content args, with
    /// the input slot position recorded for the temp-file insertion.
    struct ProbeArgv {
        std::vector<const char*> argv;
        std::size_t slot = 0;
    };

    ProbeArgv probe_argv(const CompileConfig& config);

    /// What a probe's job sets up, shared by every config of its key.
    struct Frontend {
        /// The program of the job.
        const char* driver = nullptr;

        /// The job's arguments as clice's frontend takes them.
        llvm::ArrayRef<Arg> args;

        /// The job's resource directory, whose builtin headers the command's
        /// own paths may name too.
        llvm::StringRef job_resource_dir;

        /// The job keeps its own clang resource tree: a matched LLVM-MinGW
        /// installation, whose builtin headers and libc++ belong together.
        bool own_resource_dir = false;
    };

    /// The frontend of a probe's job (the program, -cc1, its arguments):
    /// what only the probe asked for goes (its action, input and outputs),
    /// and so does what clice's frontend cannot take — flags our cc1 does
    /// not know, codegen flags, CPU and ABI names clang's target does not
    /// know. Another clang's builtin headers become ours.
    Frontend frontend(llvm::ArrayRef<std::string> job);

    /// The frontend with the config's own user content re-attached.
    ResolvedID synthesize(ConfigID id, const Frontend& probe);

    /// What the probe caches say about a (config, input) key: a live
    /// probe, a failure still cooling down, or the query to run. A
    /// cooled-down failure is dropped on the way, so the query retries.
    struct ProbeAdmission;
    ProbeAdmission admit_probe(ConfigID id, InputKind input);

    /// Commits a query's outcome: the job's frontend into the probe cache, a
    /// failure into the negative cache with its time.
    void commit_probe(llvm::StringRef key,
                      const std::expected<std::vector<std::string>, std::string>& job);

    CompilationDatabase& db;

    /// Probe layer: key → the frontend its job sets up.
    llvm::StringMap<Frontend> probes;

    /// Negative cache: keys whose query failed, mapped to the error message
    /// and when it was recorded. Avoids re-spawning the same failing driver
    /// probe for every file that shares the key (see clangd's
    /// SystemIncludeExtractor for precedent); expires after `failed_retry`.
    llvm::StringMap<std::pair<std::string, std::chrono::steady_clock::time_point>> failed;

    /// Synthesis layer: (config, input language) → resolved entry.
    llvm::DenseMap<std::pair<std::uint32_t, const char*>, ResolvedID> synth_cache;

    std::vector<Resolved> resolved_configs;

    std::chrono::steady_clock::duration failed_retry;
};

}  // namespace clice
