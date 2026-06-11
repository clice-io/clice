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
/// the compiler driver. Results are cached by (driver, file extension,
/// non-user-content flags); user-content flags (-I, -D, ...) don't affect
/// the query and are re-appended from the original command after resolution.
class Toolchain {
public:
    Toolchain();
    ~Toolchain();

    Toolchain(Toolchain&&) = default;
    Toolchain& operator=(Toolchain&&) = default;

    /// Batch pre-warm: deduplicate commands and query unique toolchains in
    /// parallel internally. Blocks until all queries complete.
    void warm(llvm::ArrayRef<CompileCommand> commands);

    /// Resolve a driver-level command to cc1 level by querying the toolchain.
    /// Modifies the command in-place.
    [[nodiscard]] std::expected<void, std::string> resolve(CompileCommand& cmd);

    /// Like resolve(), but logs a warning on failure instead of returning it.
    void resolve_or_warn(CompileCommand& cmd);

    /// Single synchronous toolchain query. Returns cc1 arguments as owned strings.
    /// `file` is used for temp file extension detection (optional if -x is set).
    /// Unlike resolve(), this is uncached and forwards `arguments` as-is; prefer
    /// resolve() for CDB commands so results are cached and per-file user-content
    /// flags are re-appended.
    static std::expected<std::vector<std::string>, std::string>
        query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file = {});

    bool has_cache() const;

    static CompilerFamily driver_family(llvm::StringRef driver);

#ifdef CLICE_ENABLE_TEST

    /// Compute the cache key for the given file and driver-level arguments.
    std::string cache_key(llvm::StringRef file, llvm::ArrayRef<const char*> arguments) {
        return extract_flags(file, arguments).key;
    }

    /// Number of cached toolchain query results.
    std::size_t cache_size() const {
        return cache.size();
    }

    /// Parse the first `-cc1` line from driver `-###` output, dropping flags
    /// our linked cc1 does not understand (along with their values).
    static std::vector<std::string> parse_cc1(llvm::StringRef content);

#endif

private:
    struct ToolchainExtract {
        std::string key;
        std::vector<const char*> query_args;
    };

    ToolchainExtract extract_flags(llvm::StringRef file, llvm::ArrayRef<const char*> arguments);

    std::unique_ptr<llvm::BumpPtrAllocator> allocator;
    StringSet strings;
    llvm::StringMap<std::vector<const char*>> cache;
};

}  // namespace clice
