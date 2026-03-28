#pragma once

#include "test/test.h"
#include "syntax/scan.h"

namespace clice::testing {

/// Helper that sets up a TestVFS with a main file and optional extra files,
/// then calls the given scan function with standard C++20 arguments.
struct ModuleScanFixture {
    llvm::IntrusiveRefCntPtr<TestVFS> vfs = llvm::makeIntrusiveRefCnt<TestVFS>();
    std::string main_path;
    std::vector<const char*> args;

    // Non-copyable/non-movable: args stores raw pointers (main_path.c_str())
    // that would dangle after copy/move.
    ModuleScanFixture(const ModuleScanFixture&) = delete;
    ModuleScanFixture& operator=(const ModuleScanFixture&) = delete;
    ModuleScanFixture(ModuleScanFixture&&) = delete;
    ModuleScanFixture& operator=(ModuleScanFixture&&) = delete;

    /// Create fixture with main file content and optional extra defines.
    ModuleScanFixture(llvm::StringRef filename,
                      llvm::StringRef content,
                      std::initializer_list<const char*> extra_args = {}) {
        main_path = TestVFS::path(filename);
        vfs->add(filename, content);
        args.push_back("clang++");
        args.push_back("-std=c++20");
        for(auto a: extra_args) {
            args.push_back(a);
        }
        args.push_back(main_path.c_str());
    }

    void add_file(llvm::StringRef name, llvm::StringRef content = "") {
        vfs->add(name, content);
    }

    ScanResult decl() {
        return scan_module_decl(args, TestVFS::root(), {}, nullptr, vfs);
    }

    ScanResult precise() {
        return scan_precise(args, TestVFS::root(), {}, nullptr, vfs);
    }
};

}  // namespace clice::testing
