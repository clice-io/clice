#include "command/invocation.h"

#include "support/filesystem.h"

#include "llvm/Support/VirtualFileSystem.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Frontend/CompilerInvocation.h"

namespace clice {

std::unique_ptr<clang::CompilerInvocation>
    create_compiler_invocation(llvm::ArrayRef<const char*> arguments,
                               llvm::StringRef directory,
                               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs,
                               llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics) {
    std::unique_ptr<clang::CompilerInvocation> invocation;
    bool is_cc1 = arguments.size() >= 2 && llvm::StringRef(arguments[1]) == "-cc1";
    if(is_cc1) {
        invocation = std::make_unique<clang::CompilerInvocation>();
        if(!clang::CompilerInvocation::CreateFromArgs(*invocation,
                                                      arguments.drop_front(2),
                                                      *diagnostics,
                                                      arguments[0])) {
            return nullptr;
        }
    } else {
        clang::CreateInvocationOptions options = {
            .Diags = std::move(diagnostics),
            .VFS = std::move(vfs),
            .ProbePrecompiled = false,
        };
        invocation = clang::createInvocation(arguments, options);
        if(!invocation) {
            return nullptr;
        }
    }

    if(!directory.empty()) {
        auto& working_dir = invocation->getFileSystemOpts().WorkingDir;
        if(working_dir.empty()) {
            working_dir = directory.str();
        } else if(!path::is_absolute(working_dir)) {
            working_dir = path::join(directory, working_dir);
        }
    }
    invocation->getFrontendOpts().DisableFree = false;
    return invocation;
}

}  // namespace clice
