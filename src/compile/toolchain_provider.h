#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A pending toolchain query, ready to be executed (possibly in parallel).
struct ToolchainQuery {
    std::string key;
    std::vector<const char*> query_args;
    llvm::StringRef file;
    llvm::StringRef directory;
};

/// Result of a toolchain query, to be injected back into the cache.
struct ToolchainResult {
    std::string key;
    std::vector<std::string> cc1_args;
};

/// Manages toolchain queries and caching, separated from CompilationDatabase.
///
/// Given compilation arguments, this component:
///   1. Extracts toolchain-relevant flags (driver, target, sysroot, stdlib, etc.)
///   2. Builds a canonical cache key from those flags
///   3. Queries the compiler driver for system include paths (expensive: spawns a process)
///   4. Caches results so identical toolchain configurations share one query
///
/// Designed to be pluggable: CompilationDatabase holds a ToolchainProvider by
/// composition and delegates all toolchain operations to it.
class ToolchainProvider {
public:
    ToolchainProvider();
    ~ToolchainProvider();
    ToolchainProvider(ToolchainProvider&&) noexcept;
    ToolchainProvider& operator=(ToolchainProvider&&) noexcept;

    /// Query toolchain with caching. Returns cached cc1 args for the given
    /// compilation arguments, running the expensive compiler query only on
    /// cache miss. The returned ArrayRef is valid for the provider's lifetime.
    llvm::ArrayRef<const char*> query_cached(llvm::StringRef file,
                                              llvm::StringRef directory,
                                              llvm::ArrayRef<const char*> arguments);

    /// Entry for batch pre-warming: file + directory + raw compilation arguments.
    struct PendingEntry {
        llvm::StringRef file;
        llvm::StringRef directory;
        llvm::SmallVector<const char*, 32> arguments;
    };

    /// Get pending queries for a batch of compilation entries.
    /// Returns queries only for cache-miss keys (deduplicated).
    std::vector<ToolchainQuery> get_pending_queries(llvm::ArrayRef<PendingEntry> entries);

    /// Inject pre-computed results into the cache. Strings are copied into
    /// the provider's internal string pool.
    void inject_results(llvm::ArrayRef<ToolchainResult> results);

    /// Check if the cache has any entries.
    bool has_cached_entries() const;

private:
    struct Impl;
    std::unique_ptr<Impl> self;
};

}  // namespace clice
