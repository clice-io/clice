#pragma once

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "support/object_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

namespace clice {

struct CompileCommand;

enum class CompilerFamily {
    Unknown,
    GCC,      // Covers gcc, g++, cc, c++, and versioned/arch variants
    Clang,    // Covers clang, clang++, and versioned variants (excluding clang-cl)
    MSVC,     // Covers cl
    ClangCL,  // Covers clang-cl explicitly
    NVCC,     // Covers nvcc
    Intel,    // Covers icc, icpc, icx, dpcpp
    Zig,      // Covers zig cc / zig c++ (assumed GCC/Clang compatible for query)
};

/// Patches raw CDB commands into clang-acceptable cc1 arguments by querying
/// the compiler driver. Results are cached by (working directory, driver, file
/// extension, non-user-content flags); user-content flags (-I, -D, ...) don't
/// affect the query and are re-appended from the original command after
/// resolution.
class Toolchain {
public:
    Toolchain();
    ~Toolchain();

    Toolchain(Toolchain&&) = default;
    Toolchain& operator=(Toolchain&&) = default;

    /// Batch pre-warm: deduplicate commands and query unique toolchains in
    /// parallel internally. Blocks until all queries complete.
    void warm(llvm::ArrayRef<CompileCommand> commands);

    /// How many probe subprocesses warm() may run at once. Defaults to the CPU
    /// parallelism and follows the configured worker-process ceiling when the
    /// server sets one. The bound matters beyond throughput: a probe forks a
    /// compiler driver and holds two pipes, and a failed spawn caches its key as
    /// permanently failed, so exhausting fds over a many-directory CDB would
    /// leave those commands without flags for the rest of the session.
    unsigned query_concurrency;

    /// Resolve a driver-level command to cc1 level by querying the toolchain.
    /// Modifies the command in-place.
    [[nodiscard]] std::expected<void, std::string> resolve(CompileCommand& cmd);

    /// Like resolve(), but logs a warning on failure instead of returning it.
    void resolve_or_warn(CompileCommand& cmd);

    /// Single synchronous toolchain query. Returns cc1 arguments as owned strings.
    /// `file` is used for temp file extension detection (optional if -x is set).
    /// `directory` is the driver's working directory: probe subprocesses (gcc
    /// -dumpmachine/-print-search-dirs, clang -###, nvcc --dryrun) run in it and a
    /// path-bearing relative `arguments[0]` resolves against it; when empty the
    /// driver inherits clice's own cwd.
    /// TODO: the GCC/MSVC/NVCC families synthesize their final cc1 through an
    /// in-process clang driver running in clice's cwd, so relative paths in their
    /// remaining flags escape `directory` — give that driver `directory` too.
    /// A bare `arguments[0]` is looked up through PATH before the child changes
    /// cwd, so a relative PATH entry anchors to clice's cwd where POSIX would
    /// anchor it to `directory`. Left as is on purpose: anchoring it means
    /// hand-rolling platform PATH lookup, for a spelling that is a security
    /// hazard and effectively unused by build systems.
    /// Unlike resolve(), this is uncached and forwards `arguments` as-is; prefer
    /// resolve() for CDB commands so results are cached and per-file user-content
    /// flags are re-appended.
    static std::expected<std::vector<std::string>, std::string>
        query(llvm::ArrayRef<const char*> arguments,
              llvm::StringRef file = {},
              llvm::StringRef directory = {});

    bool has_cache() const;

    static CompilerFamily driver_family(llvm::StringRef driver);

#ifdef CLICE_ENABLE_TEST

    /// Compute the cache key for the given file and driver-level arguments.
    std::string cache_key(llvm::StringRef file,
                          llvm::ArrayRef<const char*> arguments,
                          llvm::StringRef directory = {}) {
        return extract_flags(file, arguments, directory).key;
    }

    /// Number of cached toolchain query results.
    std::size_t cache_size() const {
        return cache.size();
    }

    /// Number of negatively cached (failed) toolchain queries.
    std::size_t failed_size() const {
        return failed.size();
    }

    /// Parse the first `-cc1` line from driver `-###` output, dropping flags
    /// our linked cc1 does not understand (along with their values).
    static std::vector<std::string> parse_cc1(llvm::StringRef content);

#endif

private:
    struct ToolchainExtract {
        std::string key;
        std::vector<const char*> query_args;

        /// The command targets windows-gnu (LLVM-MinGW): the injected
        /// -resource-dir was dropped from the query, and resolve() keeps
        /// an existing external resource tree instead of replacing it.
        bool preserve_external_resource = false;
    };

    ToolchainExtract extract_flags(llvm::StringRef file,
                                   llvm::ArrayRef<const char*> arguments,
                                   llvm::StringRef directory);

    std::unique_ptr<llvm::BumpPtrAllocator> allocator;
    StringSet strings;
    llvm::StringMap<std::vector<const char*>> cache;

    /// Negative cache: keys whose query failed, mapped to the error message.
    /// Avoids re-spawning the same failing driver probe for every file that
    /// shares the key (see clangd's SystemIncludeExtractor for precedent).
    llvm::StringMap<std::string> failed;
};

}  // namespace clice
