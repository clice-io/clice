module;

#include "modules/prelude.h"

module clice:support.process;

namespace clice {

/// What a finished program wrote, and how it ended.
struct ProgramOutput {
    std::string out;

    std::string err;

    /// How the program ended when not with exit code 0 ("Process clang
    /// ended with exit code 1"); empty when it did.
    std::string failure;
};

/// Run `arguments[0]` to completion and hand back what it wrote, however it
/// ended. Fails only when the program cannot start.
kota::task<std::expected<ProgramOutput, std::string>> execute(std::vector<std::string> arguments,
                                                              std::string cwd);

/// Hand freed heap back to the system. glibc keeps the pages of large freed
/// arenas — an AST, a TU's index, the master's merge buffers — so without
/// this a process's RSS only ever grows. The Windows allocator keeps all it
/// frees unless purge_freed_memory() was called.
void release_free_memory();

/// Let the allocator return freed memory to the system from now on, for a
/// process that holds nothing between its requests: the Windows executable
/// keeps every page it frees (src/mimalloc.cc), which spares the master and
/// a stateful worker faulting their working set back in, but leaves a
/// stateless worker at the high-water mark of its biggest request.
void purge_freed_memory();

/// Lower the calling thread's scheduling priority for the rest of its life:
/// an unprivileged thread can never raise it back, so this is for a thread
/// that ends with the background work it runs.
void lower_thread_priority();

/// Watch `signum` for good. The first delivery of any watched signal (the
/// shared `requested` flag) calls `stop` for a graceful stop. A second
/// Ctrl-C exits at once; a repeated SIGTERM or SIGHUP does not: supervisors
/// send it more than once (GNU timeout signals the child, then its whole
/// process group) and escalate with SIGKILL themselves.
kota::task<> watch_termination(int signum, bool& requested, std::function<void()> stop);

/// Whether a process with this id exists.
bool process_alive(std::uint32_t pid);

/// End this process the moment `parent` exits, or now when it already has.
void exit_with_parent(std::uint32_t parent);

/// Whether `fd` is a pipe or a socket, which Windows reports as a pipe too.
bool is_pipe_or_socket(int fd);

/// Write UTF-8 `text` to `fd` when it is a Windows console, as the UTF-16 a
/// console takes: bytes would show in its code page. False when `fd` is no
/// console, as everywhere else, `text` is no UTF-8 or the console refuses
/// it: the caller writes the bytes, and their write reports a failure.
bool write_console(int fd, llvm::StringRef text);

}  // namespace clice
