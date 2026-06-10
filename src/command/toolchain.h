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
    GCC,
    Clang,
    MSVC,
    ClangCL,
    NVCC,
    Intel,
    Zig,
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
    static std::expected<std::vector<std::string>, std::string>
        query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file = {});

    bool has_cache() const;

    static CompilerFamily driver_family(llvm::StringRef driver);

#ifdef CLICE_ENABLE_TEST

    /// Compute the cache key for the given file and driver-level arguments.
    std::string cache_key(llvm::StringRef file, llvm::ArrayRef<const char*> arguments) {
        return extract_flags(file, arguments).key;
    }

    std::size_t cache_size() const {
        return cache.size();
    }

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
