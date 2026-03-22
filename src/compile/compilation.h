#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "support/filesystem.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class CodeCompleteConsumer;

}

namespace clice {

struct PCHInfo {
    /// The path of the output PCH file.
    std::string path;

    /// The building time of this PCH.
    std::int64_t mtime;

    /// The content used to build this PCH.
    std::string preamble;

    /// All files involved in building this PCH.
    std::vector<std::string> deps;

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

    /// Files involved in building this PCM(not include module).
    std::vector<std::string> deps;
};

struct CompilationParams {
    /// The kind of this compilation.
    CompilationKind kind;

    /// Whether to run clang-tidy.
    bool clang_tidy = false;

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

}  // namespace clice
