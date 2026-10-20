module;

#include "modules/prelude.h"

module clice:support.process;

namespace clice {

/// Run `arguments[0]` to completion and hand back one of its output
/// streams: stdout when `capture_stdout`, else stderr. Fails when the
/// program cannot start or does not exit with code 0.
kota::task<std::expected<std::string, std::string>> execute(std::vector<std::string> arguments,
                                                            bool capture_stdout = false,
                                                            std::string cwd = {});

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

}  // namespace clice
