#include "compile/search_config.h"

#include "compile/driver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

using ID = clang::driver::options::ID;

SearchConfig extract_search_config(llvm::ArrayRef<const char*> arguments,
                                    llvm::StringRef directory) {
    SearchConfig config;

    auto add_dir = [&](llvm::StringRef path, bool is_system) {
        llvm::SmallString<256> abs_path(path);
        if(!llvm::sys::path::is_absolute(abs_path)) {
            llvm::sys::fs::make_absolute(directory, abs_path);
        }
        llvm::sys::path::remove_dots(abs_path, true);

        if(is_system && config.angled_start_idx == config.dirs.size()) {
            config.angled_start_idx = static_cast<unsigned>(config.dirs.size());
        }

        config.dirs.push_back({abs_path.str().str()});
    };

    llvm::BumpPtrAllocator allocator;
    ArgumentParser parser{&allocator};

    parser.parse(
        llvm::ArrayRef(arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto id = arg->getOption().getID();
            switch(id) {
                case ID::OPT_I: add_dir(arg->getValue(), false); break;
                case ID::OPT_isystem:
                case ID::OPT_internal_isystem:
                case ID::OPT_internal_externc_isystem: add_dir(arg->getValue(), true); break;
                case ID::OPT_iquote: add_dir(arg->getValue(), false); break;
                default: break;
            }
        },
        [](int, int) {});

    return config;
}

}  // namespace clice
