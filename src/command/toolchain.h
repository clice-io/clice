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
/// the compiler driver. Results are cached by (driver, extension, toolchain flags).
class Toolchain {
public:
    Toolchain();
    ~Toolchain();

    Toolchain(Toolchain&&) = default;
    Toolchain& operator=(Toolchain&&) = default;

    /// Batch pre-warm: deduplicate commands, query unique toolchains in parallel.
    /// Synchronous — internally creates an event loop for concurrent queries.
    void warm(llvm::ArrayRef<CompileCommand> commands);

    /// Resolve a driver-level command to cc1 level by querying the toolchain.
    /// Modifies the command in-place.
    std::expected<void, std::string> resolve(CompileCommand& cmd);

    /// Single synchronous toolchain query. Returns cc1 arguments as owned strings.
    /// `file` is used for temp file extension detection (optional if -x is set).
    static std::expected<std::vector<std::string>, std::string>
        query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file = {});

    bool has_cache() const;

    static CompilerFamily driver_family(llvm::StringRef driver);

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
