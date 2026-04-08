#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "support/object_pool.h"
#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"

namespace clice {

struct CommandOptions {
    /// Query the compiler driver for additional information, such as system includes and target.
    /// When enabled, also replaces the queried resource dir with our own (clang tools must use
    /// builtin headers matching their parser version — see clangd's CommandMangler for precedent).
    bool query_toolchain = false;

    /// Suppress the warning log if failed to query driver info.
    /// Set true in unittests to avoid cluttering test output.
    bool suppress_logging = false;

    /// Extra arguments to remove from the original command line.
    llvm::ArrayRef<std::string> remove;

    /// Extra arguments to append to the original command line.
    llvm::ArrayRef<std::string> append;
};

/// Resolved compilation flags — file-identity-free.
/// Contains driver/cc1 flags + patches (-I, -D, etc.) but NO source file path
/// and NO -main-file-name.  This is the "shareable" part of a compilation command,
/// directly usable as PCH cache key input and SearchConfig input without stripping.
struct ResolvedFlags {
    /// The working directory of compilation.
    llvm::StringRef directory;

    /// Compilation flags (driver + semantic flags + patches).
    /// Does NOT contain the source file path or -main-file-name.
    std::vector<const char*> flags;

    /// Whether the flags are in cc1 mode (after toolchain query).
    bool is_cc1 = false;
};

/// A file-bound compilation command = resolved flags + source file identity.
struct CompileCommand {
    ResolvedFlags resolved;

    /// The source file being compiled (interned, pointer-stable).
    const char* source_file = nullptr;

    /// Materialize a full argv for clang invocation (flags + source file appended).
    std::vector<const char*> to_argv() const {
        auto args = resolved.flags;
        if(resolved.is_cc1) {
            // Insert -main-file-name after -cc1 (or after driver if not found).
            auto basename = llvm::sys::path::filename(llvm::StringRef(source_file));
            // Find insertion point: after "-cc1" if present, else position 1.
            std::size_t pos = 1;
            if(args.size() >= 2 && args[1] == llvm::StringRef("-cc1")) {
                pos = 2;
            }
            args.insert(args.begin() + pos, {"-main-file-name", basename.data()});
        }
        args.push_back(source_file);
        return args;
    }

    /// Materialize a full argv as std::string (for worker params).
    std::vector<std::string> to_string_argv() const {
        auto args = to_argv();
        std::vector<std::string> result;
        result.reserve(args.size());
        for(auto* arg: args) {
            result.emplace_back(arg);
        }
        return result;
    }
};

/// Shared compiler identity — driver + all semantics-affecting flags.
/// Deduped via ObjectSet so most files share one instance. This directly
/// serves as the toolchain cache key (no re-parsing needed at query time).
struct CanonicalCommand {
    /// Driver path followed by semantics-affecting flags (e.g. -std=, -target, -W*).
    /// All pointers are interned in StringSet and pointer-stable.
    llvm::ArrayRef<const char*> arguments;

    friend bool operator==(const CanonicalCommand&, const CanonicalCommand&) = default;
};

/// Per-file compilation entry = shared canonical + per-file user-content patch.
/// Parsed and classified once at CDB load time; no further parsing needed.
struct CompilationInfo {
    /// Working directory (interned in StringSet, pointer-stable).
    const char* directory = nullptr;

    /// Shared canonical command (driver + semantic flags).
    object_ptr<CanonicalCommand> canonical = {nullptr};

    /// Per-file user-content options: -I, -D, -U, -include, -isystem, -iquote,
    /// -idirafter. Pre-rendered as flat arg list with -I paths already absolutized.
    llvm::ArrayRef<const char*> patch;

    friend bool operator==(const CompilationInfo&, const CompilationInfo&) = default;
};

/// A single entry in the compilation database, stored in a flat sorted vector.
struct CompilationEntry {
    /// Interned path ID for the source file (from PathPool).
    std::uint32_t file;

    /// Parsed compilation info (directory + canonical + patch).
    object_ptr<CompilationInfo> info;
};

/// A pending toolchain query, ready to be executed (possibly in parallel).
struct ToolchainQuery {
    std::string key;
    std::vector<const char*> query_args;
    std::string file;
    std::string directory;
};

/// Result of a toolchain query, to be injected back into the cache.
struct ToolchainResult {
    std::string key;
    std::vector<std::string> cc1_args;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::CanonicalCommand> {
    using T = clice::CanonicalCommand;

    inline static T getEmptyKey() {
        return T{
            llvm::ArrayRef<const char*>(reinterpret_cast<const char**>(~uintptr_t(0)), size_t(0))};
    }

    inline static T getTombstoneKey() {
        return T{llvm::ArrayRef<const char*>(reinterpret_cast<const char**>(~uintptr_t(0) - 1),
                                             size_t(0))};
    }

    static unsigned getHashValue(const T& cmd) {
        return llvm::hash_combine_range(cmd.arguments);
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        // Sentinels have distinct data pointers but both have size 0,
        // and ArrayRef equality is content-based — so we must compare
        // data pointers first to keep sentinels distinguishable.
        if(lhs.arguments.data() == rhs.arguments.data())
            return lhs.arguments.size() == rhs.arguments.size();
        if(lhs.arguments.data() == getEmptyKey().arguments.data() ||
           lhs.arguments.data() == getTombstoneKey().arguments.data() ||
           rhs.arguments.data() == getEmptyKey().arguments.data() ||
           rhs.arguments.data() == getTombstoneKey().arguments.data())
            return false;
        return lhs == rhs;
    }
};

template <>
struct DenseMapInfo<clice::CompilationInfo> {
    using T = clice::CompilationInfo;

    inline static T getEmptyKey() {
        return T{llvm::DenseMapInfo<const char*>::getEmptyKey()};
    }

    inline static T getTombstoneKey() {
        return T{llvm::DenseMapInfo<const char*>::getTombstoneKey()};
    }

    static unsigned getHashValue(const T& info) {
        return llvm::hash_combine(info.directory,
                                  info.canonical.ptr,
                                  llvm::hash_combine_range(info.patch));
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm

namespace clice {

class CompilationDatabase {
public:
    CompilationDatabase();
    ~CompilationDatabase();

    CompilationDatabase(const CompilationDatabase&) = delete;
    CompilationDatabase& operator=(const CompilationDatabase&) = delete;
    CompilationDatabase(CompilationDatabase&&) = default;
    CompilationDatabase& operator=(CompilationDatabase&&) = default;

public:
    /// Load (or reload) the compilation database from the given file.
    /// Full reload: old entries are replaced, SearchConfig cache is cleared,
    /// but toolchain cache survives. Returns the number of entries loaded.
    std::size_t load(llvm::StringRef path);

    /// Lookup the compilation commands for a file. A file may have multiple
    /// compilation commands (e.g. different build configurations); all are returned.
    /// The returned CompileCommand separates resolved flags from source file identity.
    llvm::SmallVector<CompileCommand> lookup(llvm::StringRef file,
                                             const CommandOptions& options = {});

    /// Combined lookup + extract_search_config with internal caching.
    SearchConfig lookup_search_config(llvm::StringRef file, const CommandOptions& options = {});

    /// Check if SearchConfig cache is populated (non-empty).
    bool has_cached_configs() const;

    /// Resolve a path_id back to the file path string.
    llvm::StringRef resolve_path(std::uint32_t path_id);

    /// Intern a file path and return its path_id.
    std::uint32_t intern_path(llvm::StringRef path);

    /// Check if a file has an explicit entry in the compilation database
    /// (as opposed to a synthesized default).
    bool has_entry(llvm::StringRef file);

    /// All compilation entries (sorted by path_id).
    llvm::ArrayRef<CompilationEntry> get_entries() const;

    /// Entry for batch pre-warming: file + directory + raw compilation arguments.
    struct PendingEntry {
        llvm::StringRef file;
        llvm::StringRef directory;
        llvm::SmallVector<const char*, 32> arguments;
    };

    /// Get pending toolchain queries for a batch of compilation entries.
    /// Returns queries only for cache-miss keys (deduplicated).
    std::vector<ToolchainQuery> get_pending_queries(llvm::ArrayRef<PendingEntry> entries);

    /// Inject pre-computed toolchain results into the cache. Strings are copied
    /// into the internal string pool.
    void inject_results(llvm::ArrayRef<ToolchainResult> results);

    /// Check if toolchain cache has any entries.
    bool has_cached_toolchain() const;

#ifdef CLICE_ENABLE_TEST

    void add_command(llvm::StringRef directory,
                     llvm::StringRef file,
                     llvm::ArrayRef<const char*> arguments);

    void add_command(llvm::StringRef directory, llvm::StringRef file, llvm::StringRef command);

#endif

private:
    /// Find all CompilationEntry items for a file by path_id (binary search).
    /// Returns a sub-range of `entries`; may be empty.
    llvm::ArrayRef<CompilationEntry> find_entries(std::uint32_t path_id) const;

    /// Allocate a persistent copy of a const char* array on the bump allocator.
    llvm::ArrayRef<const char*> persist_args(llvm::ArrayRef<const char*> args);

    /// Parse and classify a compilation command into canonical + patch.
    object_ptr<CompilationInfo> save_compilation_info(llvm::StringRef file,
                                                      llvm::StringRef directory,
                                                      llvm::ArrayRef<const char*> arguments);

    object_ptr<CompilationInfo> save_compilation_info(llvm::StringRef file,
                                                      llvm::StringRef directory,
                                                      llvm::StringRef command);

    static std::uint8_t options_bits(const CommandOptions& options) {
        return options.query_toolchain ? 1u : 0u;
    }

    struct ToolchainExtract {
        std::string key;
        std::vector<const char*> query_args;
    };

    /// Extract toolchain-relevant flags and build a cache key.
    ToolchainExtract extract_toolchain_flags(llvm::StringRef file,
                                             llvm::ArrayRef<const char*> arguments);

    /// Query toolchain with caching. Returns cached cc1 args, running the
    /// expensive compiler query only on cache miss.
    llvm::ArrayRef<const char*> query_toolchain_cached(llvm::StringRef file,
                                                       llvm::StringRef directory,
                                                       llvm::ArrayRef<const char*> arguments);

    /// The memory pool which holds all elements of compilation database.
    /// Heap-allocated so its address is stable across moves.
    std::unique_ptr<llvm::BumpPtrAllocator> allocator = std::make_unique<llvm::BumpPtrAllocator>();

    /// Keep all strings (arguments, directories, etc.).
    StringSet strings{allocator.get()};

    /// Shared canonical commands — most files share one instance.
    ObjectSet<CanonicalCommand> canonicals{allocator.get()};

    /// Per-file compilation infos (canonical + patch + directory).
    ObjectSet<CompilationInfo> infos{allocator.get()};

    /// Intern pool for file paths → compact uint32_t IDs.
    PathPool paths;

    /// All compilation entries, sorted by file path_id.
    /// Multiple entries for the same file are adjacent.
    std::vector<CompilationEntry> entries;

    /// Cache of SearchConfig keyed by (CompilationInfo*, options_bits).
    using ConfigCacheKey = std::pair<const CompilationInfo*, std::uint8_t>;
    llvm::DenseMap<ConfigCacheKey, SearchConfig> search_config_cache;

    /// Cache of toolchain query results, keyed by canonical toolchain key.
    llvm::StringMap<std::vector<const char*>> toolchain_cache;

    std::unique_ptr<ArgumentParser> parser = std::make_unique<ArgumentParser>(allocator.get());
};

}  // namespace clice
