module clice;

import :command.invocation;
import :vfs.path;

namespace clice {

std::unique_ptr<clang::CompilerInvocation>
    create_compiler_invocation(llvm::ArrayRef<const char*> arguments,
                               llvm::StringRef directory,
                               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs,
                               llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diagnostics) {
    // The compile runs in its directory: files the driver checks and the
    // frontend opens straight through the file system (sanitizer lists, VFS
    // overlays) resolve there, as in the real build.
    if(vfs && !directory.empty()) {
        vfs->setCurrentWorkingDirectory(directory);
    }
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
        working_dir = path::join(directory, working_dir);
    }
    invocation->getFrontendOpts().DisableFree = false;
    return invocation;
}

std::uint64_t module_compatibility(const clang::CompilerInvocation& invocation) {
    using Kind = clang::LangOptions::CompatibilityKind;
    auto& language = invocation.getLangOpts();
    llvm::SmallVector<std::uint64_t, 256> values;
#define LANGOPT(Name, Bits, Default, Compatibility, Description)                                   \
    if constexpr(Kind::Compatibility == Kind::NotCompatible) {                                     \
        values.push_back(language.Name);                                                           \
    }
#define ENUM_LANGOPT(Name, Type, Bits, Default, Compatibility, Description)                        \
    if constexpr(Kind::Compatibility == Kind::NotCompatible) {                                     \
        values.push_back(static_cast<std::uint64_t>(language.get##Name()));                        \
    }
#include "clang/Basic/LangOptions.def"

    std::string buffer(reinterpret_cast<const char*>(values.data()),
                       values.size() * sizeof(std::uint64_t));
    auto& target = invocation.getTargetOpts();
    llvm::SmallVector<llvm::StringRef> features(target.FeaturesAsWritten.begin(),
                                                target.FeaturesAsWritten.end());
    ranges::sort(features);
    auto append = [&](llvm::StringRef part) {
        buffer += part;
        buffer += '\0';
    };
    append(target.Triple);
    append(target.ABI);
    ranges::for_each(features, append);
    return llvm::xxh3_64bits(buffer);
}

}  // namespace clice
