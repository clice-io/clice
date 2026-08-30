#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "command/command.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Resolves driver-level configs into full cc1-level configurations by
/// probing the compiler driver, in two layers:
///
///   - Probe layer: keyed by (driver, input language, non-user-content
///     args, and — only for cwd-sensitive configs — the directory). One
///     driver spawn per unique key; results and failures (with a retry
///     cooldown) are cached. User-content flags (-I, -D, ...) never affect
///     the probe and are re-attached at synthesis.
///
///   - Synthesis layer: keyed by (config, input). Probe output parsed once
///     into structured args, the config's own user-content re-attached,
///     external resource dirs replaced with ours (except a matched
///     LLVM-MinGW installation, which keeps its own tree).
class Toolchain {
public:
    using ResolvedID = std::uint32_t;

    /// A synthesized full compile configuration, self-contained: rendering
    /// it needs no trip back to the source config.
    struct Resolved {
        /// The compiler binary the probe resolved.
        const char* driver = nullptr;

        /// Frontend-level args (cc1): render as `driver -cc1 <args>`.
        /// False for driver-level results (cl mode).
        bool is_cc1 = false;

        llvm::ArrayRef<Arg> args;
    };

    /// `failed_retry` bounds the negative cache: a failed key is retried
    /// once the cooldown passes (cf. CrashBudget), so a transient driver
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

    /// Pre-probe in parallel, deduplicated by probe key. Blocks until all
    /// unique probes complete.
    void warm(llvm::ArrayRef<std::pair<ConfigID, InputKind>> pairs);

    bool has_cache() const {
        return !probes.empty();
    }

    static CompilerFamily driver_family(llvm::StringRef driver);

    /// Single synchronous toolchain query on a raw driver argv (no input
    /// file among the arguments; a temp input with `file`'s extension is
    /// appended). Uncached — prefer resolve() for CDB configs.
    static std::expected<std::vector<std::string>, std::string>
        query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file = {});

#ifdef CLICE_ENABLE_TEST

    std::string probe_key_for(ConfigID id, InputKind input) {
        return probe_key(id, input).key;
    }

    void set_failed_retry(std::chrono::steady_clock::duration retry) {
        failed_retry = retry;
    }

    bool probe_cwd_sensitive_for(ConfigID id) {
        return probe_key(id, {}).cwd_sensitive;
    }

    std::size_t probe_count() const {
        return probes.size();
    }

    std::size_t failed_count() const {
        return failed.size();
    }

    /// Parse the first `-cc1` line from driver `-###` output, dropping flags
    /// our linked cc1 does not understand (along with their values).
    static std::vector<std::string> parse_cc1(llvm::StringRef content);

#endif

private:
    struct ProbeKey {
        std::string key;
        bool cwd_sensitive = false;
    };

    ProbeKey probe_key(ConfigID id, InputKind input);

    /// The probe argv: driver (+ subcommand) + non-user-content args, with
    /// the input slot position recorded for the temp-file insertion.
    /// Relative path-suspect values of cwd-sensitive configs absolutize
    /// against the directory (the in-process driver cannot change cwd).
    struct ProbeArgv {
        std::vector<const char*> argv;
        std::size_t slot = 0;
    };

    ProbeArgv probe_argv(const CompileConfig& config, bool cwd_sensitive);

    /// Parse raw probe output into a Resolved entry for `id`, re-attaching
    /// the config's user-content args and replacing external resource dirs.
    ResolvedID synthesize(ConfigID id, llvm::ArrayRef<const char*> tokens);

    CompilationDatabase& db;

    /// Probe layer: key → raw probe output tokens (interned).
    llvm::StringMap<llvm::SmallVector<const char*, 64>> probes;

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
