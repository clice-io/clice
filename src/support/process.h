#pragma once

#include <expected>
#include <string>
#include <vector>

#include "kota/async/async.h"

namespace clice {

/// Run `arguments[0]` to completion and hand back one of its output
/// streams: stdout when `capture_stdout`, else stderr. Fails when the
/// program cannot start or does not exit with code 0.
kota::task<std::expected<std::string, std::string>> execute(std::vector<std::string> arguments,
                                                            bool capture_stdout = false,
                                                            std::string cwd = {});

/// Hand freed heap back to the system. glibc keeps the pages of large freed
/// arenas — an AST, a TU's index, the master's merge buffers — so without
/// this a process's RSS only ever grows.
void release_free_memory();

/// Lower the calling thread's scheduling priority for the rest of its life:
/// an unprivileged thread can never raise it back, so this is for a thread
/// that ends with the background work it runs.
void lower_thread_priority();

}  // namespace clice
