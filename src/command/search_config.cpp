module;

#include "modules/prelude.h"

module clice;

import :command.argument;
import :command.search_config;
import :vfs.path;

namespace clice {

using namespace option;

SearchConfig extract_search_config(llvm::ArrayRef<Arg> args, llvm::StringRef directory) {
    // clang's layout (ParseHeaderSearchArgs, then InitHeaderSearch::Realize):
    // Quoted (-iquote) → Angled (-I, then -iwithprefixbefore) → System
    // (-isystem, then the driver's -internal-isystem) → After (-iwithprefix,
    // then -idirafter). A group keeps the command's order within each kind
    // of option, wherever the options sit in the command.
    std::vector<SearchDir> quoted;
    std::vector<SearchDir> angled;
    std::vector<SearchDir> angled_prefixed;
    std::vector<SearchDir> system;
    std::vector<SearchDir> system_driver;
    std::vector<SearchDir> after_prefixed;
    std::vector<SearchDir> after;
    std::vector<std::string> forced_includes;

    // A leading `=` names the sysroot: the last -isysroot, else the last
    // --sysroot.
    std::string_view isysroot;
    std::string_view sysroot;
    for(auto& arg: args) {
        if(arg.values.empty()) {
            continue;
        }
        if(arg.opt_id == OPT_isysroot) {
            isysroot = arg.values[0];
        } else if(arg.opt_id == OPT__sysroot_EQ) {
            sysroot = arg.values[0];
        }
    }
    if(!isysroot.empty()) {
        sysroot = isysroot;
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
            case OPT_iquote: quoted.push_back({make_absolute(value)}); break;
            case OPT_I: angled.push_back({make_absolute(value)}); break;
            case OPT_isystem: system.push_back({make_absolute(value)}); break;
            case OPT_internal_isystem:
            case OPT_internal_externc_isystem:
                system_driver.push_back({.path = make_absolute(value), .driver = true});
                break;

            // Prefix options: must be processed in argument order.
            case OPT_iprefix: prefix = value; break;
            case OPT_iwithprefix:
                after_prefixed.push_back({make_absolute(prefix + std::string(value))});
                break;
            case OPT_iwithprefixbefore:
                angled_prefixed.push_back({make_absolute(prefix + std::string(value))});
                break;

            case OPT_idirafter: after.push_back({make_absolute(value)}); break;

            case OPT_include: forced_includes.emplace_back(value); break;

            // FIXME: what clang's HeaderSearch has and this does not:
            // framework directories (-F, -iframework, -internal-iframework),
            // C++-only system directories (-cxx-isystem), -iwithsysroot,
            // and header maps (-I foo.hmap remaps include names).
            default: break;
        }
    }

    SearchConfig config;
    config.forced_includes = std::move(forced_includes);
    auto append = [&](std::vector<SearchDir>& group) {
        config.dirs.insert(config.dirs.end(),
                           std::make_move_iterator(group.begin()),
                           std::make_move_iterator(group.end()));
    };
    append(quoted);
    config.angled_start_idx = static_cast<unsigned>(config.dirs.size());
    append(angled);
    append(angled_prefixed);
    config.system_start_idx = static_cast<unsigned>(config.dirs.size());
    append(system);
    append(system_driver);
    append(after_prefixed);
    append(after);

    // Duplicates go as clang's RemoveDuplicates takes them out, Quoted
    // apart from the rest: the later copy, except that a user directory
    // (-I) the system groups repeat goes in favor of the system copy, as
    // in GCC — the order #include_next walks. Duplicates are one directory
    // however spelled, as clang tells them apart by the directory itself.
    std::vector<bool> removed(config.dirs.size());
    auto remove_duplicates = [&](unsigned first, unsigned end) {
        llvm::StringMap<unsigned> seen;
        for(unsigned i = first; i < end; i += 1) {
            auto key = CanonicalPath(Spelling::absolute(config.dirs[i].path)).str();
            auto [it, inserted] = seen.try_emplace(key, i);
            if(inserted) {
                continue;
            }
            if(i >= config.system_start_idx && it->second < config.system_start_idx) {
                removed[it->second] = true;
                it->second = i;
            } else {
                removed[i] = true;
            }
        }
    };
    remove_duplicates(0, config.angled_start_idx);
    remove_duplicates(config.angled_start_idx, static_cast<unsigned>(config.dirs.size()));

    unsigned write = 0;
    unsigned angled_start = config.angled_start_idx;
    unsigned system_start = config.system_start_idx;
    for(unsigned read = 0; read < config.dirs.size(); read += 1) {
        if(removed[read]) {
            angled_start -= read < config.angled_start_idx ? 1 : 0;
            system_start -= read < config.system_start_idx ? 1 : 0;
            continue;
        }
        if(write != read) {
            config.dirs[write] = std::move(config.dirs[read]);
        }
        write += 1;
    }
    config.dirs.resize(write);
    config.angled_start_idx = angled_start;
    config.system_start_idx = system_start;
    return config;
}

}  // namespace clice
