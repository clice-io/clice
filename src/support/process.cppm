module clice:support.process;

import :support.prelude;

namespace clice {

/// What a finished program wrote, and how it ended.
struct ProgramOutput {
    std::string out;

    std::string err;

    /// How the program ended when not with exit code 0 ("Process clang
    /// ended with exit code 1"); empty when it did.
    std::string failure;
};

/// Whether a child can be made to die with this process
/// (CreationOptions::exit_with_parent): macOS has nothing to do it with.
#ifdef __APPLE__
constexpr inline bool can_exit_with_parent = false;
#else
constexpr inline bool can_exit_with_parent = true;
#endif

/// Run `arguments[0]` to completion and hand back what it wrote, however it
/// ended. Fails only when the program cannot start. A `path` given is the
/// program's PATH.
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
/// covers every thread's arena at once.
async::task<> release_pool_memory();

/// Watch `signum` for good. The first delivery of any watched signal (the
/// shared `requested` flag) calls `stop` for a graceful stop. A second
/// Ctrl-C exits at once; a repeated SIGTERM or SIGHUP does not: supervisors
/// send it more than once (GNU timeout signals the child, then its whole
/// process group) and follow with SIGKILL themselves.
async::task<> watch_termination(int signum, bool& requested, std::function<void()> stop);

#ifdef __APPLE__
/// End this process the moment its parent exits, or now when it already
/// has: a child started with CreationOptions::exit_with_parent elsewhere.
void exit_with_parent();
#endif

}  // namespace clice
