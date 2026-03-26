#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/search_config.h"
#include "command/toolchain_provider.h"
#include "support/object_pool.h"
#include "support/path_pool.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct CommandOptions {
    /// Ignore unknown commands arguments.
    bool ignore_unknown = true;

    /// Query the compiler driver for additional information, such as system includes and target.
    /// When enabled, also replaces the queried resource dir with our own (clang tools must use
    /// builtin headers matching their parser version — see clangd's CommandMangler for precedent).
    bool query_toolchain = false;

    /// Suppress the warning log if failed to query driver info.
    /// Set true in unittests to avoid cluttering test output.
    bool suppress_logging = false;

    /// The commands that you want to remove from original commands list.
    llvm::ArrayRef<std::string> remove;

    /// The commands that you want to add to original commands list.
    llvm::ArrayRef<std::string> append;
};

struct CompilationContext {
    /// The working directory of compilation.
    llvm::StringRef directory;

    /// The compilation arguments.
    std::vector<const char*> arguments;
};

using StringID = StringSet::ID;

/// Shared compiler identity — driver + all semantics-affecting flags.
/// Deduped via ObjectSet so most files share one instance. This directly
/// serves as the toolchain cache key (no re-parsing needed at query time).
struct CanonicalCommand {
    llvm::ArrayRef<StringID> arguments;
    friend bool operator==(const CanonicalCommand&, const CanonicalCommand&) = default;
};

/// Per-file compilation entry = shared canonical + per-file user-content patch.
/// Parsed and classified once at CDB load time; no further parsing needed.
struct CompilationInfo {
    StringID directory = 0;

    /// Shared canonical command (driver + semantic flags).
    object_ptr<CanonicalCommand> canonical = {nullptr};

    /// Per-file user-content options: -I, -D, -U, -include, -isystem, -iquote,
    /// -idirafter. Pre-rendered as flat arg list with -I paths already absolutized.
    llvm::ArrayRef<StringID> patch;

    friend bool operator==(const CompilationInfo&, const CompilationInfo&) = default;
};

/// A single entry in the compilation database, stored in a flat sorted vector.
struct CompilationEntry {
    /// Interned path ID for the source file (from PathPool).
    std::uint32_t file;

    /// Parsed compilation info (directory + canonical + patch).
    object_ptr<CompilationInfo> info;
};

std::string print_argv(llvm::ArrayRef<const char*> args);

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<clice::CanonicalCommand> {
    using T = clice::CanonicalCommand;

    inline static T getEmptyKey() {
        return T{llvm::ArrayRef<clice::StringID>(reinterpret_cast<clice::StringID*>(~uintptr_t(0)),
                                                 size_t(0))};
    }

    inline static T getTombstoneKey() {
        return T{
            llvm::ArrayRef<clice::StringID>(reinterpret_cast<clice::StringID*>(~uintptr_t(0) - 1),
                                            size_t(0))};
    }

    static unsigned getHashValue(const T& cmd) {
        return llvm::hash_combine_range(cmd.arguments);
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

template <>
struct DenseMapInfo<clice::CompilationInfo> {
    using T = clice::CompilationInfo;

    inline static T getEmptyKey() {
        return T{llvm::DenseMapInfo<std::uint32_t>::getEmptyKey()};
    }

    inline static T getTombstoneKey() {
        return T{llvm::DenseMapInfo<std::uint32_t>::getTombstoneKey()};
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
    /// but ToolchainProvider cache survives. Returns the number of entries loaded.
    std::size_t load(llvm::StringRef path);

    /// Lookup the compilation context for a file. When a file has multiple
    /// compilation commands, returns the first one.
    CompilationContext lookup(llvm::StringRef file, const CommandOptions& options = {});

    /// Combined lookup + extract_search_config with internal caching.
    SearchConfig lookup_search_config(llvm::StringRef file, const CommandOptions& options = {});

    /// Check if SearchConfig cache is populated (non-empty).
    bool has_cached_configs() const;

    /// Get the option ID for a specific argument string.
    static std::optional<std::uint32_t> get_option_id(llvm::StringRef argument);

    /// Get the resource directory for clang builtin headers. Computed once
    /// from the current executable path using Driver::GetResourcesPath.
    static llvm::StringRef resource_dir();

    /// Resolve a path_id back to the file path string.
    llvm::StringRef resolve_path(std::uint32_t path_id);

    /// Access the toolchain provider for batch pre-warming and direct queries.
    ToolchainProvider& toolchain();

#ifdef CLICE_ENABLE_TEST

    void add_command(llvm::StringRef directory,
                     llvm::StringRef file,
                     llvm::ArrayRef<const char*> arguments);

    void add_command(llvm::StringRef directory, llvm::StringRef file, llvm::StringRef command);

#endif

private:
    /// Find the CompilationInfo for a file by its path_id (binary search).
    object_ptr<CompilationInfo> find_info(std::uint32_t path_id) const;

    /// Options that are completely irrelevant to an LSP and should be discarded.
    static bool is_discarded_option(unsigned id);

    /// User-content options go into the per-file patch (not the shared canonical).
    static bool is_user_content_option(unsigned id);

    /// Render a parsed argument into a flat list of StringIDs.
    void render_arg(llvm::SmallVectorImpl<StringID>& out, llvm::opt::Arg& arg);

    /// Render a parsed argument into a flat list of const char*.
    void render_arg_chars(std::vector<const char*>& out, llvm::opt::Arg& arg);

    /// Allocate a persistent copy of a StringID array on the bump allocator.
    llvm::ArrayRef<StringID> persist_ids(llvm::ArrayRef<StringID> ids);

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

    /// Pluggable toolchain provider: manages toolchain queries and caching.
    ToolchainProvider toolchain_;

    /// Cache of SearchConfig keyed by (CompilationInfo*, options_bits).
    using ConfigCacheKey = std::pair<const CompilationInfo*, std::uint8_t>;
    llvm::DenseMap<ConfigCacheKey, SearchConfig> search_config_cache;

    std::unique_ptr<ArgumentParser> parser = std::make_unique<ArgumentParser>(allocator.get());
};

}  // namespace clice
