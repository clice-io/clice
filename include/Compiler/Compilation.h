#pragma once

#include "Module.h"
#include "Preamble.h"
#include "CompilationUnit.h"
#include "Support/FileSystem.h"

namespace clang {
class CodeCompleteConsumer;
}

namespace clice {

struct CompilationParams {
    /// The kind of this compilation.
    CompilationUnit::Kind kind;

    /// Whether to run clang-tidy.
    bool clang_tidy = false;

    /// Output file path.
    llvm::SmallString<128> output_file;

    std::string directory;

    /// Responsible for storing the arguments.
    std::vector<const char*> arguments;

    llvm::IntrusiveRefCntPtr<vfs::FileSystem> vfs = new ThreadSafeFS();

    /// Information about reuse PCH.
    std::pair<std::string, uint32_t> pch;

    /// Information about reuse PCM(name, path).
    llvm::StringMap<std::string> pcms;

    /// Code completion file:offset.
    std::tuple<std::string, std::uint32_t> completion;

    /// The memory buffers for all remapped file.
    llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> buffers;

    /// A flag to inform to stop compilation, this is very useful
    /// to cancel old compilation task.
    std::shared_ptr<std::atomic_bool> stop = std::make_shared<std::atomic_bool>(false);

    /// Store all compilation errors in the process.
    std::shared_ptr<std::vector<Diagnostic>> diagnostics;

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

using CompilationResult = std::expected<CompilationUnit, std::string>;

/// Only preprocess ths source flie.
CompilationResult preprocess(CompilationParams& params);

/// Build AST from given file path and content. If pch or pcm provided, apply them to the compiler.
/// Note this function will not check whether we need to update the PCH or PCM, caller should check
/// their reusability and update in time.
CompilationResult compile(CompilationParams& params);

/// Build PCH from given file path and content.
CompilationResult compile(CompilationParams& params, PCHInfo& out);

/// Build PCM from given file path and content.
CompilationResult compile(CompilationParams& params, PCMInfo& out);

/// Run code completion at the given location.
CompilationResult complete(CompilationParams& params, clang::CodeCompleteConsumer* consumer);

}  // namespace clice
