#include "compile/search_config.h"

#include "compile/driver.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

using ID = clang::driver::options::ID;

SearchConfig extract_search_config(llvm::ArrayRef<const char*> arguments,
                                    llvm::StringRef directory) {
    // Replicate clang's InitHeaderSearch::Realize layout:
    //   Quoted (-iquote) → Angled (-I) → System (-isystem, -internal-isystem, etc.)
    // Then deduplicate across [Angled..end) matching clang's RemoveDuplicates.

    std::vector<SearchDir> quoted;
    std::vector<SearchDir> angled;
    std::vector<SearchDir> system;

    auto make_absolute = [&](llvm::StringRef path) -> std::string {
        llvm::SmallString<256> abs_path(path);
        if(!llvm::sys::path::is_absolute(abs_path)) {
            llvm::sys::fs::make_absolute(directory, abs_path);
        }
        llvm::sys::path::remove_dots(abs_path, true);
        return abs_path.str().str();
    };

    // Track -iprefix state for -iwithprefix/-iwithprefixbefore.
    std::string prefix;

    llvm::BumpPtrAllocator allocator;
    ArgumentParser parser{&allocator};

    parser.parse(
        llvm::ArrayRef(arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto id = arg->getOption().getID();
            switch(id) {
                // Quoted group (clang: frontend::Quoted)
                case ID::OPT_iquote:
                    quoted.push_back({make_absolute(arg->getValue())});
                    break;

                // Angled group (clang: frontend::Angled)
                case ID::OPT_I:
                    angled.push_back({make_absolute(arg->getValue())});
                    break;

                // System group (clang: frontend::System / ExternCSystem)
                case ID::OPT_isystem:
                case ID::OPT_internal_isystem:
                case ID::OPT_internal_externc_isystem:
                    system.push_back({make_absolute(arg->getValue())});
                    break;

                // Prefix options: must be processed in argument order.
                case ID::OPT_iprefix:
                    prefix = arg->getValue();
                    break;
                case ID::OPT_iwithprefix:
                    // clang maps to After group; we simplify After→System.
                    system.push_back({make_absolute(prefix + arg->getValue())});
                    break;
                case ID::OPT_iwithprefixbefore:
                    // clang maps to Angled group.
                    angled.push_back({make_absolute(prefix + arg->getValue())});
                    break;

                // TODO: -idirafter (clang: frontend::After group, searched after System)
                // TODO: HeaderMap support
                default: break;
            }
        },
        [](int, int) {});

    // Concatenate: Quoted → Angled → System
    SearchConfig config;
    config.dirs.reserve(quoted.size() + angled.size() + system.size());
    config.dirs.insert(config.dirs.end(), std::make_move_iterator(quoted.begin()),
                       std::make_move_iterator(quoted.end()));
    config.angled_start_idx = static_cast<unsigned>(config.dirs.size());
    config.dirs.insert(config.dirs.end(), std::make_move_iterator(angled.begin()),
                       std::make_move_iterator(angled.end()));
    config.system_start_idx = static_cast<unsigned>(config.dirs.size());
    config.dirs.insert(config.dirs.end(), std::make_move_iterator(system.begin()),
                       std::make_move_iterator(system.end()));

    // Deduplicate across [angled_start_idx..end), matching clang's
    // RemoveDuplicates(SearchList, NumQuoted). If a path appears in both
    // Angled and System, keep the first (Angled) occurrence. This is
    // critical for #include_next correctness.
    {
        llvm::StringSet<> seen;
        // Seed with Quoted paths (they're not deduped against Angled/System).
        for(unsigned i = 0; i < config.angled_start_idx; ++i) {
            seen.insert(config.dirs[i].path);
        }

        unsigned write = config.angled_start_idx;
        unsigned system_removed_before = 0;
        for(unsigned read = config.angled_start_idx; read < config.dirs.size(); ++read) {
            if(seen.insert(config.dirs[read].path).second) {
                if(write != read) {
                    config.dirs[write] = std::move(config.dirs[read]);
                }
                ++write;
            } else {
                // Track removals before system_start_idx to adjust it.
                if(read < config.system_start_idx) {
                    ++system_removed_before;
                }
            }
        }
        config.dirs.resize(write);
        config.system_start_idx -= system_removed_before;
    }

    return config;
}

}  // namespace clice
