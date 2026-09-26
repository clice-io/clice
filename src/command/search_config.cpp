#include "command/search_config.h"

#include "command/argument_parser.h"
#include "command/command.h"
#include "support/filesystem.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

using namespace option;

SearchConfig extract_search_config(llvm::ArrayRef<Arg> args, llvm::StringRef directory) {
    // Replicate clang's InitHeaderSearch::Realize layout:
    //   Quoted (-iquote) → Angled (-I) → System (-isystem, -internal-isystem, etc.)
    // Then deduplicate across [Angled..end) matching clang's RemoveDuplicates.

    std::vector<SearchDir> quoted;
    std::vector<SearchDir> angled;
    std::vector<SearchDir> system;
    std::vector<SearchDir> after;

    // A leading `=` names the sysroot: -isysroot, else --sysroot.
    std::string_view sysroot;
    for(auto& arg: args) {
        if(arg.values.empty()) {
            continue;
        }
        if(arg.opt_id == OPT_isysroot) {
            sysroot = arg.values[0];
        } else if(sysroot.empty() && (arg.opt_id == OPT__sysroot_EQ || arg.opt_id == OPT__sysroot)) {
            sysroot = arg.values[0];
        }
    }
    auto base = Spelling::absolute(directory);
    auto make_absolute = [&](std::string_view path) -> std::string {
        if(path.starts_with('=') && !sysroot.empty()) {
            return Spelling(std::string(sysroot) + std::string(path.substr(1)), base).str();
        }
        return Spelling(path, base).str();
    };

    // Track -iprefix state for -iwithprefix/-iwithprefixbefore.
    std::string prefix;

    for(auto& arg: args) {
        if(arg.values.empty()) {
            continue;
        }
        std::string_view value = arg.values[0];
        switch(arg.opt_id) {
            // Quoted group (clang: frontend::Quoted)
            case OPT_iquote: quoted.push_back({make_absolute(value)}); break;

            // Angled group (clang: frontend::Angled)
            case OPT_I: angled.push_back({make_absolute(value)}); break;

            // System group (clang: frontend::System / ExternCSystem)
            case OPT_isystem:
            case OPT_internal_isystem:
            case OPT_internal_externc_isystem: system.push_back({make_absolute(value)}); break;

            // Prefix options: must be processed in argument order.
            case OPT_iprefix: prefix = value; break;
            case OPT_iwithprefix:
                // clang maps to After group.
                after.push_back({make_absolute(prefix + std::string(value))});
                break;
            case OPT_iwithprefixbefore:
                // clang maps to Angled group.
                angled.push_back({make_absolute(prefix + std::string(value))});
                break;

            case OPT_idirafter: after.push_back({make_absolute(value)}); break;

            // TODO: -cxx-isystem (clang: frontend::CXXSystem, C++-only system dirs)
            // TODO: -iwithsysroot (prepends sysroot to path, then adds to System)
            // TODO: HeaderMap support (-I foo.hmap remaps include names)
            default: break;
        }
    }

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
    // Duplicates are one directory however spelled, as clang tells them
    // apart by the directory itself.
    {
        llvm::StringSet<> seen;
        // Do NOT seed with Quoted paths. clang's RemoveDuplicates(SearchList,
        // NumQuoted) starts from NumQuoted, so a path in both Quoted and Angled
        // is kept in both — this matters for #include <...> and #include_next.

        unsigned write = config.angled_start_idx;
        unsigned removed_before_system = 0;
        unsigned removed_before_after = 0;
        for(unsigned read = config.angled_start_idx; read < config.dirs.size(); ++read) {
            if(seen.insert(CanonicalPath(Spelling::absolute(config.dirs[read].path)).str()).second) {
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
