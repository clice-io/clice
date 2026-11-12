module clice:support.process;

import :support.prelude;

namespace clice {

/// The value of the environment variable `name`; nullopt when it is unset.
inline std::optional<std::string> env_string(llvm::StringRef name) {
    std::string value;
    if(!sys::get_env(value, name)) {
        return std::nullopt;
    }
    return value;
}

/// The integer an environment variable holds: nullopt when it is unset or
/// holds no integer.
inline std::optional<std::uint64_t> env_integer(llvm::StringRef name) {
    std::uint64_t parsed = 0;
    auto value = env_string(name);
    if(!value || llvm::StringRef(*value).getAsInteger(10, parsed)) {
        return std::nullopt;
    }
    return parsed;
}

/// The threads of libuv's pool, which runs async::queue's work: as libuv
/// sizes it from UV_THREADPOOL_SIZE when it is first used, clamped to
/// [1, 1024], 4 without it.
inline std::size_t thread_pool_size() {
    return std::clamp<std::uint64_t>(env_integer("UV_THREADPOOL_SIZE").value_or(4), 1, 1024);
}

/// Test hook: CLICE_TEST_JITTER=<ms> makes this a random delay of up to that
/// many milliseconds, zero while it is unset. The places that race with an
/// editor's actions (a worker starting a request's work, a background index
/// pass starting) wait it out, so a run of the cases under it meets the
/// orders a loaded machine meets; a case whose answer changes left an order
/// unpinned.
inline chrono::milliseconds test_jitter() {
    const static auto max = env_integer("CLICE_TEST_JITTER");
    if(!max) {
        return chrono::milliseconds(0);
    }
    thread_local std::minstd_rand random(std::random_device{}());
    return chrono::milliseconds(std::uniform_int_distribution<std::uint64_t>(0, *max)(random));
}

/// What a finished program wrote, and how it ended.
struct ProgramOutput {
    std::string out;

    std::string err;

    /// How the program ended when not with exit code 0 ("Process clang
    /// ended with exit code 1"); empty when it did.
    std::string failure;
};

/// Whether a child can be made to die with this process
/// (CreationOptions::exit_with_parent): macOS has nothing to do it with, but
/// a child calling sys::exit_with_parent itself.
#ifdef __APPLE__
constexpr inline bool can_exit_with_parent = false;
#else
constexpr inline bool can_exit_with_parent = true;
#endif

/// Run `arguments[0]` to completion and hand back what it wrote, however it
/// ended. Fails when the program cannot start, and when it has not ended
/// within 30 s: it is then killed with what it started. A `path` given is
/// the program's PATH.
async::task<std::expected<ProgramOutput, std::string>> execute(std::vector<std::string> arguments,
                                                               std::string cwd,
                                                               std::string path = {});

/// Hand freed heap back to the system. glibc keeps the pages of large freed
/// arenas — an AST, a TU's index, the master's merge buffers — so without
/// this a process's RSS only ever grows; mimalloc returns them a second
/// after they were freed (src/mimalloc.cc), or at this collect.
void release_free_memory();

/// release_free_memory on every thread of libuv's pool, then on this one.
/// mimalloc (src/mimalloc.cc) collects only the calling thread's heap, and a
/// block freed on another thread waits in its owner's page until the owner
/// allocates or collects: an AST a pool thread built and the loop thread
/// freed stays committed while that pool thread is idle. The collects wait
/// for each other so that each runs on a thread of its own; a thread still
/// busy after a moment is left to its next collect. glibc's malloc_trim
/// covers every thread's arena at once, on one pool thread: on a large heap
/// it takes long enough to stall the loop. A call while one runs returns at
/// once and has it run again, for what was freed meanwhile.
async::task<> release_free_memory_everywhere();

/// Watch `signum` for good. The first delivery of any watched signal (the
/// shared `requested` flag) calls `stop` for a graceful stop. A second
/// Ctrl-C exits at once; a repeated SIGTERM or SIGHUP does not: supervisors
/// send it more than once (GNU timeout signals the child, then its whole
/// process group) and follow with SIGKILL themselves.
async::task<> watch_termination(int signum, bool& requested, std::function<void()> stop);

}  // namespace clice
