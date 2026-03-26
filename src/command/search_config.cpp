#include "command/search_config.h"

#include "command/argument_parser.h"

#include "clang/Driver/Options.h"

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
    std::vector<SearchDir> after;

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
                case ID::OPT_iquote: quoted.push_back({make_absolute(arg->getValue())}); break;

                // Angled group (clang: frontend::Angled)
                case ID::OPT_I: angled.push_back({make_absolute(arg->getValue())}); break;

                // System group (clang: frontend::System / ExternCSystem)
                case ID::OPT_isystem:
                case ID::OPT_internal_isystem:
                case ID::OPT_internal_externc_isystem:
                    system.push_back({make_absolute(arg->getValue())});
                    break;

                // Prefix options: must be processed in argument order.
                case ID::OPT_iprefix: prefix = arg->getValue(); break;
                case ID::OPT_iwithprefix:
                    // clang maps to After group.
                    after.push_back({make_absolute(prefix + arg->getValue())});
                    break;
                case ID::OPT_iwithprefixbefore:
                    // clang maps to Angled group.
                    angled.push_back({make_absolute(prefix + arg->getValue())});
                    break;

                case ID::OPT_idirafter: after.push_back({make_absolute(arg->getValue())}); break;

                // TODO: -cxx-isystem (clang: frontend::CXXSystem, C++-only system dirs)
                // TODO: -iwithsysroot (prepends sysroot to path, then adds to System)
                // TODO: HeaderMap support (-I foo.hmap remaps include names)
                default: break;
            }
        },
        [](int, int) {});

    // Concatenate: Quoted → Angled → System → After
    SearchConfig config;
    config.dirs.reserve(quoted.size() + angled.size() + system.size() + after.size());
    config.dirs.insert(config.dirs.end(),
                       std::make_move_iterator(quoted.begin()),
                       std::make_move_iterator(quoted.end()));
    config.angled_start_idx = static_cast<unsigned>(config.dirs.size());
    config.dirs.insert(config.dirs.end(),
                       std::make_move_iterator(angled.begin()),
                       std::make_move_iterator(angled.end()));
    config.system_start_idx = static_cast<unsigned>(config.dirs.size());
    config.dirs.insert(config.dirs.end(),
                       std::make_move_iterator(system.begin()),
                       std::make_move_iterator(system.end()));
    config.after_start_idx = static_cast<unsigned>(config.dirs.size());
    config.dirs.insert(config.dirs.end(),
                       std::make_move_iterator(after.begin()),
                       std::make_move_iterator(after.end()));

    // Deduplicate across [angled_start_idx..end), matching clang's
    // RemoveDuplicates(SearchList, NumQuoted). If a path appears in both
    // Angled and System, keep the first (Angled) occurrence. This is
    // critical for #include_next correctness.
    {
        llvm::StringSet<> seen;
        // Do NOT seed with Quoted paths. clang's RemoveDuplicates(SearchList,
        // NumQuoted) starts from NumQuoted, so a path in both Quoted and Angled
        // is kept in both — this matters for #include <...> and #include_next.

        unsigned write = config.angled_start_idx;
        unsigned removed_before_system = 0;
        unsigned removed_before_after = 0;
        for(unsigned read = config.angled_start_idx; read < config.dirs.size(); ++read) {
            if(seen.insert(config.dirs[read].path).second) {
                if(write != read) {
                    config.dirs[write] = std::move(config.dirs[read]);
                }
                ++write;
            } else {
                if(read < config.system_start_idx) {
                    ++removed_before_system;
                }
                if(read < config.after_start_idx) {
                    ++removed_before_after;
                }
            }
        }
        config.dirs.resize(write);
        config.system_start_idx -= removed_before_system;
        config.after_start_idx -= removed_before_after;
    }

    return config;
}

}  // namespace clice
