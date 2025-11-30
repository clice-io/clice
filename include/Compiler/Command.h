#pragma once

#include <expected>

#include "Toolchain.h"
#include "Support/Enum.h"
#include "Support/Format.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Allocator.h"

namespace clice {

struct CommandOptions {
    /// Ignore unknown commands arguments.
    bool ignore_unknown = true;

    /// Inject resource directory to the command.
    bool resource_dir = false;

    /// Query the compiler driver for additional information, such as system includes and target.
    bool query_toolchain = false;

    /// Suppress the warning log if failed to query driver info.
    /// Set true in unittests to avoid cluttering test output.
    bool suppress_logging = false;

    /// The commands that you want to remove from original commands list.
    llvm::ArrayRef<std::string> remove;

    /// The commands that you want to add to original commands list.
    llvm::ArrayRef<std::string> append;
};

enum class UpdateKind : std::uint8_t {
    Unchanged,
    Inserted,
    Deleted,
};

struct UpdateInfo {
    /// The kind of update.
    UpdateKind kind;

    /// The updated file.
    std::uint32_t path_id;

    /// The compilation context of this file command, which could
    /// be used to identity the same file with different compilation
    /// contexts.
    const void* context;
};

struct CompilationContext {
    /// The working directory of compilation.
    llvm::StringRef directory;

    /// The compilation arguments.
    std::vector<const char*> arguments;
};

std::string print_argv(llvm::ArrayRef<const char*> args);

class CompilationDatabase {
public:
    CompilationDatabase();

    CompilationDatabase(const CompilationDatabase&) = delete;

    CompilationDatabase(CompilationDatabase&& other);

    CompilationDatabase& operator=(const CompilationDatabase&) = delete;

    CompilationDatabase& operator=(CompilationDatabase&& other);

    ~CompilationDatabase();

public:
    /// Read the compilation database on the give file and return the
    /// incremental update infos.
    std::vector<UpdateInfo> load_compile_database(llvm::StringRef file);

    /// Lookup the compilation context of specific file. If the context
    /// param is provided, we will return the compilation context corresponding
    /// to the handle. Otherwise we just return the first one(if the file have)
    /// multiple compilation contexts.
    CompilationContext lookup(llvm::StringRef file,
                              const CommandOptions& options = {},
                              const void* context = nullptr);

    /// TODO: list all compilation context of the file, this is useful to show
    /// all contexts and let user choose one.
    /// std::vector<CompilationContext> fetch_all(llvm::StringRef file);

    /// Get an the option for specific argument.
    static std::optional<std::uint32_t> get_option_id(llvm::StringRef argument);

    /// FIXME: bad interface design ...
    std::vector<const char*> files();

    /// FIXME: remove this api?
    auto save_string(llvm::StringRef string) -> llvm::StringRef;

#ifdef CLICE_ENABLE_TEST

    void add_command(llvm::StringRef directory,
                     llvm::StringRef file,
                     llvm::ArrayRef<const char*> arguments);

    void add_command(llvm::StringRef directory, llvm::StringRef file, llvm::StringRef command);

    /// FIXME: remove this
    /// Update commands from json file and return all updated file.
    std::expected<std::vector<UpdateInfo>, std::string> load_commands(llvm::StringRef json_content,
                                                                      llvm::StringRef workspace);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> self;
};

}  // namespace clice
