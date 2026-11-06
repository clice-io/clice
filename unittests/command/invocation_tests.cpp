module clice;

import :command.invocation;
import :unittests.utils;
import :vfs.file_system;

namespace clice::testing {

namespace {

ZEST_SUITE(ModuleCompatibility) {

ModuleCompatibility compatibility(std::vector<const char*> arguments) {
    arguments.insert(arguments.begin(), {"clang++", "--target=x86_64-unknown-linux-gnu"});
    arguments.push_back("-fsyntax-only");
    auto vfs = llvm::makeIntrusiveRefCnt<vfs::View>();
    clang::DiagnosticOptions options;
    auto diagnostics = clang::CompilerInstance::createDiagnostics(*vfs,
                                                                  options,
                                                                  new clang::IgnoringDiagConsumer(),
                                                                  true);
    auto invocation = create_compiler_invocation(arguments, "", vfs, diagnostics);
    ZASSERT(invocation != nullptr);
    return module_compatibility(*invocation);
}

ZEST_CASE(MacrosAndFilesIgnored) {
    /// An importer reads a module whatever its macros, include paths and
    /// file kind.
    auto module = compatibility({"-std=c++20", "m.cppm"});
    ZEXPECT(compatibility({"-std=c++20", "-DVALUE=1", "-Iinclude", "user.cpp"}).reads(module));
}

ZEST_CASE(LanguageOptionsCount) {
    auto module = compatibility({"-std=c++20", "m.cppm"});
    ZEXPECT(!compatibility({"-std=c++23", "user.cpp"}).reads(module));
    ZEXPECT(!compatibility({"-std=c++20", "-pthread", "user.cpp"}).reads(module));
    ZEXPECT(!compatibility({"-std=c++20", "-fno-rtti", "user.cpp"}).reads(module));
}

ZEST_CASE(TargetCounts) {
    auto module = compatibility({"-std=c++20", "m.cppm"});
    ZEXPECT(!compatibility({"-std=c++20", "--target=aarch64-unknown-linux-gnu", "user.cpp"})
                 .reads(module));
}

ZEST_CASE(ImporterAddsFeatures) {
    /// clang reads a module into a compile with more target features, not
    /// fewer.
    auto module = compatibility({"-std=c++20", "m.cppm"});
    auto avx = compatibility({"-std=c++20", "-mavx", "user.cpp"});
    ZEXPECT(avx.reads(module));
    ZEXPECT(!module.reads(avx));
}

};  // ZEST_SUITE(ModuleCompatibility)

}  // namespace

}  // namespace clice::testing
