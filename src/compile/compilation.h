#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "compile/dep_file.h"
#include "support/filesystem.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class CodeCompleteConsumer;

}

namespace clice::tidy {

/// The frozen clang-tidy configuration of one run. Plain data on purpose:
/// it travels in worker requests and in the TURun family's product plan,
/// both of which freeze it at takeoff.
struct TidyParams {
    /// Effective checks glob list in clang-tidy syntax; empty falls back
    /// to the built-in default set.
    std::string checks;

    /// Restrict the run to checks classified fast — the interactive
    /// path's latency guard. Batch lint runs everything configured.
    bool fast_only = true;

    /// Check options (the .clang-tidy CheckOptions map).
    std::vector<std::pair<std::string, std::string>> options;

    /// Checks whose findings report as errors (WarningsAsErrors globs).
    std::string warnings_as_errors;

    /// Also report findings in headers matching this regex (clang-tidy's
    /// HeaderFilterRegex; empty = main file only).
    std::string header_filter;

    /// Headers excluded from reporting even when header_filter matches
    /// (clang-tidy's ExcludeHeaderFilterRegex).
    std::string exclude_header_filter;

    /// Also report findings in system headers.
    bool system_headers = false;

    /// Extra compiler args from the configuration. -W<group> flags are
    /// consumed engine-side by apply_warning_options so the Checks gate
    /// applies (the clangd approach, see tidy.cpp); a batch lint run
    /// additionally applies the remaining args (command_extra_args) to
    /// its driver command at resolution, as clang-tidy itself does. The
    /// interactive command is never rewritten.
    std::vector<std::string> extra_args;
    std::vector<std::string> extra_args_before;
};

/// Resolve the effective clang-tidy configuration for `file` from its
/// nearest .clang-tidy files (clang-tidy's own search and inheritance
/// semantics). Files without any configuration return empty checks — the
/// consumer's default set applies.
TidyParams resolve_tidy_params(llvm::StringRef file);

/// The plan's compilation-affecting extra args, split for clang-tidy's
/// own insertion points on the driver command: extra_args_before prepend
/// right after the binary name, extra_args append at the end. Applied
/// BEFORE toolchain resolution, so the driver itself interprets
/// pass-throughs and driver-only options (-Wp,, -Xpreprocessor,
/// --target) when it produces the cc1 line.
struct CommandExtraArgs {
    std::vector<std::string> prepend;
    std::vector<std::string> append;
};

/// Split the plan's extra args into the command-affecting halves. -W
/// warning flags are withheld — they reach the diagnostics engine through
/// apply_warning_options, where the Checks gate applies; on the command
/// they would bypass it. -Wp,/-Wl,/-Wa, are driver pass-throughs, not
/// warning flags, and stay in.
CommandExtraArgs command_extra_args(llvm::ArrayRef<std::string> extra_args,
                                    llvm::ArrayRef<std::string> extra_args_before);

}  // namespace clice::tidy

namespace clice {

struct PCHInfo {
    /// The path of the output PCH file.
    std::string path;

    /// The content used to build this PCH.
    std::string preamble;

    /// All files involved in building this PCH, with consumed-content hashes.
    std::vector<DepFile> deps;

    /// The command arguments used to build this PCH.
    std::vector<const char*> arguments;
};

struct ModuleInfo {
    /// Whether this module is an interface unit.
    /// i.e. has export module declaration.
    bool isInterfaceUnit = false;

    /// Module name.
    std::string name;

    /// Dependent modules of this module.
    std::vector<std::string> mods;
};

struct PCMInfo : ModuleInfo {
    /// PCM file path.
    std::string path;

    /// Source file path.
    std::string srcPath;

    /// Files involved in building this PCM (not including imported modules),
    /// with consumed-content hashes. Contains the module source file itself:
    /// unlike the PCH key, the PCM cache key does not embed any content, so
    /// the deps snapshot is the only thing that can see the source change.
    std::vector<DepFile> deps;
};

struct CompilationParams {
    /// The kind of this compilation.
    CompilationKind kind;

    /// Run clang-tidy over the parse with this frozen configuration.
    std::optional<tidy::TidyParams> tidy;

    /// Whether to collect the syntax::TokenBuffer during the run. Features
    /// need it; measurement paths turn it off to isolate its cost.
    bool collect_tokens = true;

    /// Output file path.
    llvm::SmallString<128> output_file;

    std::string directory;

    /// Responsible for storing the arguments.
    std::vector<const char*> arguments;

    llvm::IntrusiveRefCntPtr<vfs::FileSystem> vfs = new ThreadSafeFS();

    /// Information about reuse PCH.
    std::pair<std::string, std::uint32_t> pch;

    /// Information about reuse PCM(name, path).
    llvm::StringMap<std::string> pcms;

    /// Code completion file:offset.
    std::tuple<std::string, std::uint32_t> completion;

    /// The memory buffers for all remapped file.
    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;

    /// A flag to inform to stop compilation, this is very useful
    /// to cancel old compilation task.
    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);

    void add_remapped_file(llvm::StringRef path,
                           llvm::StringRef content,
                           std::uint32_t bound = -1) {
        if(bound != -1) {
            assert(bound <= content.size());
            content = content.substr(0, bound);
        }
        buffers.try_emplace(path, llvm::MemoryBuffer::getMemBufferCopy(content));
    }
};

/// Only preprocess ths source flie.
CompilationUnit preprocess(CompilationParams& params);

/// Build AST from given file path and content. If pch or pcm provided, apply them to the compiler.
/// Note this function will not check whether we need to update the PCH or PCM, caller should check
/// their reusability and update in time.
CompilationUnit compile(CompilationParams& params);

/// Build PCH from given file path and content.
CompilationUnit compile(CompilationParams& params, PCHInfo& out);

/// Build PCM from given file path and content.
CompilationUnit compile(CompilationParams& params, PCMInfo& out);

/// Run code completion at the given location.
CompilationUnit complete(CompilationParams& params, clang::CodeCompleteConsumer* consumer);

/// Error-level diagnostics of a finished compilation, joined with "; ".
std::string collect_errors(CompilationUnit& unit);

}  // namespace clice
