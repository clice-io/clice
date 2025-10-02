#pragma once

#include <expected>

#include "Support/Enum.h"
#include "Support/Format.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Allocator.h"

namespace clice {

struct CommandOptions {
    /// Ignore unknown commands.
    bool ignore_unknown = true;

    /// The commands that you want to remove from original commands list.
    llvm::ArrayRef<std::string> remove;

    /// The commands that you want to add to original commands list.
    llvm::ArrayRef<std::string> append;

    /// Attach resource directory to the command.
    bool resource_dir = false;

    /// Query the compiler driver for additional information, such as system includes and target.
    bool query_driver = false;

    /// Suppress the warning log if failed to query driver info.
    /// Set true in unittests to avoid cluttering test output.
    bool suppress_logging = false;
};

class CompilationDatabase {
public:
    using Self = CompilationDatabase;

    enum class UpdateKind : std::uint8_t {
        Unchange,
        Create,
        Update,
        Delete,
    };

    struct CommandInfo {
        /// TODO: add sysroot or no stdinc command info.
        llvm::StringRef directory;

        /// The canonical command list.
        llvm::ArrayRef<const char*> arguments;

        /// The extra command @...
        llvm::StringRef response_file;

        /// The original index of the response file argument in the command list.
        std::uint32_t response_file_index = 0;
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

        llvm::StringRef file;
    };

    struct LookupInfo {
        llvm::StringRef directory;

        std::vector<const char*> arguments;
    };

    struct QueryDriverError {
        struct ErrorKind : refl::Enum<ErrorKind> {
            enum Kind : std::uint8_t {
                NotFoundInPATH,
                FailToCreateTempFile,
                InvokeDriverFail,
                OutputFileNotReadable,
                InvalidOutputFormat,
            };

            using Enum::Enum;
        };

        ErrorKind kind;
        std::string detail;
    };

    CompilationDatabase();

    CompilationDatabase(CompilationDatabase&& other);

    CompilationDatabase& operator= (CompilationDatabase&& other);

    ~CompilationDatabase();

    auto save_string(this Self& self, llvm::StringRef string) -> llvm::StringRef;

    auto save_cstring_list(this Self& self, llvm::ArrayRef<const char*> arguments)
        -> llvm::ArrayRef<const char*>;

    /// Get an the option for specific argument.
    static std::optional<std::uint32_t> get_option_id(llvm::StringRef argument);

    /// Query the compiler driver and return its driver info.
    auto query_driver(this Self& self, llvm::StringRef driver)
        -> std::expected<DriverInfo, QueryDriverError>;

    /// Update with arguments.
    auto update_command(this Self& self,
                        llvm::StringRef directory,
                        llvm::StringRef file,
                        llvm::ArrayRef<const char*> arguments) -> UpdateInfo;

    /// Update with full command.
    auto update_command(this Self& self,
                        llvm::StringRef directory,
                        llvm::StringRef file,
                        llvm::StringRef command) -> UpdateInfo;

    /// Update commands from json file and return all updated file.
    auto load_commands(this Self& self, llvm::StringRef json_content, llvm::StringRef workspace)
        -> std::expected<std::vector<UpdateInfo>, std::string>;

    auto process_command(this Self& self,
                         llvm::StringRef file,
                         const CommandInfo& info,
                         const CommandOptions& options) -> std::vector<const char*>;

    /// Get compile command from database. `file` should has relative path of workspace.
    auto get_command(this Self& self, llvm::StringRef file, CommandOptions options = {})
        -> LookupInfo;

    /// Load compile commands from given directories. If no valid commands are found,
    /// search recursively from the workspace directory.
    auto load_compile_database(this Self& self,
                               llvm::ArrayRef<std::string> compile_commands_dirs,
                               llvm::StringRef workspace) -> void;

private:
    /// If file not found in CDB file, try to guess commands or use the default case.
    auto guess_or_fallback(this Self& self, llvm::StringRef file) -> LookupInfo;

private:
    /// The opaque handle of `ArgumentParser`.
    void* parser;

    /// The memory pool to hold all cstring and command list.
    llvm::BumpPtrAllocator allocator;

    /// A cache between input string and its cache cstring
    /// in the allocator, make sure end with `\0`.
    llvm::DenseSet<llvm::StringRef> string_cache;

    /// A cache between input command and its cache array
    /// in the allocator.
    llvm::DenseSet<llvm::ArrayRef<const char*>> arguments_cache;

    /// The clang options we want to filter in all cases, like -c and -o.
    llvm::DenseSet<std::uint32_t> filtered_options;

    /// A map between file path and its canonical command list.
    llvm::DenseMap<const char*, CommandInfo> command_infos;

    /// A map between driver path and its query driver info.
    llvm::DenseMap<const char*, DriverInfo> driver_infos;
};

}  // namespace clice

namespace llvm {

template <>
struct DenseMapInfo<llvm::ArrayRef<const char*>> {
    using T = llvm::ArrayRef<const char*>;

    inline static T getEmptyKey() {
        return T(reinterpret_cast<T::const_pointer>(~0), T::size_type(0));
    }

    inline static T getTombstoneKey() {
        return T(reinterpret_cast<T::const_pointer>(~1), T::size_type(0));
    }

    static unsigned getHashValue(const T& value) {
        return llvm::hash_combine_range(value.begin(), value.end());
    }

    static bool isEqual(const T& lhs, const T& rhs) {
        return lhs == rhs;
    }
};

}  // namespace llvm

template <>
struct std::formatter<clice::CompilationDatabase::QueryDriverError> :
    std::formatter<llvm::StringRef> {

    template <typename FormatContext>
    auto format(clice::CompilationDatabase::QueryDriverError& e, FormatContext& ctx) const {
        return std::format_to(ctx.out(), "{} {}", e.kind.name(), e.detail);
    }
};
