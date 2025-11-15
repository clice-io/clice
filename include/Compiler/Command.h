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
    bool query_driver = false;

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

struct DriverInfo {
    /// The target of this driver.
    llvm::StringRef target;

    /// The default system includes of this driver.
    llvm::ArrayRef<const char*> system_includes;
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

struct LookupInfo {
    llvm::StringRef directory;

    std::vector<const char*> arguments;

    /// The include arguments indices in the arguments list.
    std::vector<std::uint32_t> include_indices;
};

class CompilationDatabase {
public:
    CompilationDatabase();

    CompilationDatabase(const CompilationDatabase&) = delete;

    CompilationDatabase(CompilationDatabase&& other);

    CompilationDatabase& operator= (const CompilationDatabase&) = delete;

    CompilationDatabase& operator= (CompilationDatabase&& other);

    ~CompilationDatabase();

private:
    struct Impl;

public:
    /// Add a command to the compilation database. This function is mainly
    ///  used in the unit tests.
    void add_command(llvm::StringRef file, llvm::StringRef directory, llvm::StringRef command);

    /// Read the compilation database on the give file and return the
    /// incremental update infos.
    std::vector<UpdateInfo> load_compile_database(llvm::StringRef file);

    /// Lookup the compilation context of specific file. If the context
    /// param is provided, we will return the compilation context corresponding
    /// to the handle. Otherwise we just return the first one(if the file have)
    /// multiple compilation contexts.
    CompilationContext lookup(llvm::StringRef file,
                              const void* context = nullptr,
                              const CommandOptions& options = {});

    /// TODO: list all compilation context of the file, this is useful to show
    /// all contexts and let user choose one.
    /// std::vector<CompilationContext> fetch_all(llvm::StringRef file);

    /// Get an the option for specific argument.
    static std::optional<std::uint32_t> get_option_id(llvm::StringRef argument);

    std::vector<const char*> files();

    auto save_string(llvm::StringRef string) -> llvm::StringRef;

    /// Query the compiler driver and return its driver info.
    auto query_driver(llvm::StringRef driver)
        -> std::expected<DriverInfo, toolchain::QueryDriverError>;

    auto query_toolchain(llvm::ArrayRef<const char*> arguments) -> std::vector<const char*>;

    /// Update with arguments.
    auto update_command(llvm::StringRef directory,
                        llvm::StringRef file,
                        llvm::ArrayRef<const char*> arguments) -> UpdateInfo;

    /// Update with full command.
    auto update_command(llvm::StringRef directory, llvm::StringRef file, llvm::StringRef command)
        -> UpdateInfo;

    /// Update commands from json file and return all updated file.
    std::expected<std::vector<UpdateInfo>, std::string> load_commands(llvm::StringRef json_content,
                                                                      llvm::StringRef workspace);

    /// Load compile commands from given directories. If no valid commands are found,
    /// search recursively from the workspace directory.
    void load_compile_database(llvm::ArrayRef<std::string> compile_commands_dirs,
                               llvm::StringRef workspace);

    /// Get compile command from database. `file` should has relative path of workspace.
    auto lookup(llvm::StringRef file, CommandOptions options = {}) -> LookupInfo;

private:
    std::unique_ptr<Impl> self;
};

}  // namespace clice
