module clice;

import :command.invocation;
import :unittests.utils;
import :vfs.file_system;

namespace clice::testing {

namespace {

ZEST_SUITE(ModuleCompatibility) {

std::uint64_t compatibility(std::vector<const char*> arguments) {
    arguments.insert(arguments.begin(), {"clang++", "--target=x86_64-unknown-linux-gnu"});
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
    auto base = compatibility({"-std=c++20", "-fsyntax-only", "m.cppm"});
    ZEXPECT(base ==
            compatibility({"-std=c++20", "-DVALUE=1", "-Iinclude", "-fsyntax-only", "user.cpp"}));
}

ZEST_CASE(LanguageOptionsCount) {
    auto base = compatibility({"-std=c++20", "-fsyntax-only", "m.cppm"});
    ZEXPECT(base != compatibility({"-std=c++23", "-fsyntax-only", "m.cppm"}));
    ZEXPECT(base != compatibility({"-std=c++20", "-pthread", "-fsyntax-only", "m.cppm"}));
    ZEXPECT(base != compatibility({"-std=c++20", "-fno-rtti", "-fsyntax-only", "m.cppm"}));
}

ZEST_CASE(TargetCounts) {
    auto base = compatibility({"-std=c++20", "-fsyntax-only", "m.cppm"});
    ZEXPECT(base !=
            compatibility(
                {"-std=c++20", "--target=aarch64-unknown-linux-gnu", "-fsyntax-only", "m.cppm"}));
}

};  // ZEST_SUITE(ModuleCompatibility)

}  // namespace

}  // namespace clice::testing
