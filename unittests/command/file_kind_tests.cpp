module clice;

import :command.file_kind;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(FileKind) {

ZEST_CASE(ModuleUnitSuffixes) {
    // What other compilers and build systems name module units, which
    // clang's driver knows nothing of, is a C++ source.
    namespace types = clang::driver::types;
    for(llvm::StringRef path: {"a.ixx", "a.mxx", "a.mpp"}) {
        ZEXPECT(suffix_type(path) == types::TY_CXX);
        ZEXPECT(driver_type(path) == types::TY_INVALID);
        ZEXPECT(is_source_path(path));
        ZEXPECT(is_c_family_file(path));
    }
}

};  // ZEST_SUITE(FileKind)

}  // namespace

}  // namespace clice::testing
