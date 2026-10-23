module;

#include "support/prelude.h"

module clice:command.toolchain;

import :command.argument;
import :command.search_config;
import :command.table;
import :vfs.file_table;

namespace clice {

/// Resolves driver-level commands into full cc1-level configurations by
/// querying the compiler driver, in two layers:
///
///   - Query layer: keyed by (driver, input language, the args that may
///     change the driver's job, and — only for cwd-sensitive commands — the
///     directory). One query per unique key finds the cc1 job the driver
///     runs; its frontend (see Frontend) and failures (with a retry
///     cooldown) are cached. User-content flags (-I, -D, ...) never affect
///     the query.
///
///   - Synthesis layer: keyed by (command, input). The query's frontend with
///     the command's own user content re-attached.
class Toolchain {
public:
    using ResolvedID = std::uint32_t;

    /// A synthesized full compile configuration, self-contained: rendering
    /// it needs no trip back to the source command.
    struct Resolved {
        /// The compiler binary the query resolved.
        const char* driver = nullptr;

        /// Frontend-level args (cc1): render as `driver -cc1 <args>`.
        llvm::ArrayRef<Arg> args;
    };

    /// `failed_retry` bounds the negative cache: a failed key is retried
    /// once the cooldown passes (cf. BlameBudget), so a transient driver
    /// failure — an upgrade replacing the binary mid-stat, a full tmpfs —
    /// cannot poison the key for the rest of the session.
    Toolchain(CommandTable& commands,
              FileTable& files,
              std::chrono::steady_clock::duration failed_retry = std::chrono::seconds(30));
    ~Toolchain();

    /// A command resolved for one input language.
    struct ResolvedCommand {
        ResolvedID id;

        /// Why the compiler could not be queried, which leaves `id` clang's
        /// own driver's reading of the command; empty when it answered.
        std::string query_failure;
    };

    /// Resolve a command for one input language. Blocks on a driver spawn on
    /// query miss; hits are lookups. A compiler that cannot be queried is
    /// read as a rule's `query_toolchain = false` reads it, by clang's own
    /// driver; an error only when that driver cannot read the command
    /// either.
    std::expected<ResolvedCommand, std::string> resolve(CommandID id, InputKind input);

    const Resolved& resolved(ResolvedID id) const {
        return resolved_configs[id];
    }

    /// Pre-query in parallel, deduplicated by query key, running as many
    /// drivers at once as the process has CPUs. Blocks until all unique
    /// queries complete.
    void warm(llvm::ArrayRef<CommandRef> refs);

    /// A compile argv and how the command came to it.
    struct RenderedCommand {
        std::vector<const char*> argv;

        /// ResolvedCommand::query_failure of the resolution `argv` renders.
        std::string query_failure;
    };

    /// Render the full compile argv for a ref, naming the input `path`:
    /// resolve the command through the toolchain (query cached; may spawn
    /// the driver once per unique toolchain key) and render at cc1 level;
    /// when even clang's own driver cannot read the command, degrade to
    /// the driver-level render.
    RenderedCommand render(const CommandRef& ref,
                           llvm::StringRef path,
                           const RenderOptions& opts = {});

    /// The driver command clang's own driver reads in place of the compiler
    /// of `ref` (read_as_clang), naming the input `path`: what `clice
    /// command` shows of a command whose compiler could not be queried, and
    /// the compile's last resort when that driver builds no job.
    std::vector<const char*> render_for_clang(const CommandRef& ref,
                                              llvm::StringRef path,
                                              const RenderOptions& opts = {});

    /// Header search configuration of a ref, extracted from the resolved
    /// (cc1) command — memoized — else from the driver-level command.
    SearchConfig search_config(const CommandRef& ref);

    bool has_cache() const {
        return !queries.empty();
    }

    std::string query_key_for(CommandID id, InputKind input) {
        return query_key(id, input).key;
    }

    void set_failed_retry(std::chrono::steady_clock::duration retry) {
        failed_retry = retry;
    }

    std::size_t query_count() const {
        return queries.size();
    }

    std::size_t failed_count() const {
        return failed.size();
    }

private:
    struct QueryKey {
        std::string key;
        bool cwd_sensitive = false;
    };

    QueryKey query_key(CommandID id, InputKind input);

    /// The query argv: driver (+ subcommand) + non-user-content args, with
    /// the input slot position recorded for the temp-file insertion.
    struct QueryArgv {
        std::vector<const char*> argv;
        std::size_t slot = 0;
    };

    QueryArgv query_argv(const Command& command);

    /// What a query's job sets up, shared by every command of its key.
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

    /// The frontend of a query's job (the program, -cc1, its arguments):
    /// what only the query asked for goes (its action, input and outputs),
    /// and so does what clice's frontend cannot take — flags our cc1 does
    /// not know, codegen flags, CPU and ABI names clang's target does not
    /// know. Another clang's builtin headers become ours.
    Frontend frontend(llvm::ArrayRef<std::string> job);

    /// The frontend with the command's own user content re-attached.
    ResolvedID synthesize(CommandID id, const Frontend& query);

    /// The command read by clang's own driver in place of a compiler that
    /// could not be queried (`failure` says why), as a rule's
    /// `query_toolchain = false` reads it.
    std::expected<ResolvedCommand, std::string> read_unqueried(CommandID id,
                                                               InputKind input,
                                                               std::string failure);

    /// What the query caches say about a (command, input) key: a live
    /// query, a failure still cooling down, or the query to run. A
    /// cooled-down failure is dropped on the way, so the query retries.
    struct QueryAdmission;
    QueryAdmission admit_query(CommandID id, InputKind input);

    /// Commits a query's outcome: the job's frontend into the query cache, a
    /// failure into the negative cache with its time.
    void commit_query(llvm::StringRef key,
                      const std::expected<std::vector<std::string>, std::string>& job);

private:
    CommandTable& commands;

    FileTable& files;

    /// Query layer: key → the frontend its job sets up.
    llvm::StringMap<Frontend> queries;

    /// Negative cache: keys whose query failed, mapped to the error message
    /// and when it was recorded. Avoids re-spawning the same failing driver
    /// query for every file that shares the key (see clangd's
    /// SystemIncludeExtractor for precedent); expires after `failed_retry`.
    llvm::StringMap<std::pair<std::string, std::chrono::steady_clock::time_point>> failed;

    /// Synthesis layer: (command, input language) → resolved entry.
    llvm::DenseMap<std::pair<std::uint32_t, const char*>, ResolvedID> synth_cache;

    std::vector<Resolved> resolved_configs;

    llvm::DenseMap<std::uint32_t, SearchConfig> search_configs;

    std::chrono::steady_clock::duration failed_retry;
};

}  // namespace clice
