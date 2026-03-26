#include "command/toolchain_provider.h"

#include "command/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "clang/Driver/Options.h"

namespace clice {

using ID = clang::driver::options::ID;

bool ToolchainProvider::is_excluded_option(unsigned id) {
    switch(id) {
        case ID::OPT_I:
        case ID::OPT_isystem:
        case ID::OPT_iquote:
        case ID::OPT_idirafter:
        case ID::OPT_D:
        case ID::OPT_U:
        case ID::OPT_include:
        case ID::OPT_INPUT: return true;
        default: return false;
    }
}

ToolchainProvider::ToolchainExtract
    ToolchainProvider::extract_toolchain_flags(llvm::StringRef file,
                                               llvm::ArrayRef<const char*> arguments) {
    ToolchainExtract result;

    // Driver binary (first arg) — e.g. "clang++" vs "clang" affects language mode.
    result.key += arguments[0];
    result.key += '\0';

    // File extension affects language mode (C vs C++).
    result.key += path::extension(file);
    result.key += '\0';

    result.query_args.push_back(arguments[0]);

    parser.parse(
        llvm::ArrayRef(arguments).drop_front(),
        [&](std::unique_ptr<llvm::opt::Arg> arg) {
            auto& opt = arg->getOption();
            auto id = opt.getID();
            if(is_excluded_option(id) || is_codegen_option(id, opt)) {
                return;
            }

            // Add option ID and all its values to the cache key.
            result.key += std::to_string(id);
            result.key += '\0';
            for(auto value: arg->getValues()) {
                result.key += value;
                result.key += '\0';
            }

            // Render the argument back to query args, respecting the option's
            // render style (joined vs separate).
            switch(arg->getOption().getRenderStyle()) {
                case llvm::opt::Option::RenderJoinedStyle: {
                    // e.g. -std=c++17, --target=x86_64-linux-gnu
                    llvm::SmallString<64> joined(arg->getSpelling());
                    if(arg->getNumValues() > 0) {
                        joined += arg->getValue(0);
                    }
                    result.query_args.push_back(strings.save(joined).data());
                    break;
                }
                case llvm::opt::Option::RenderSeparateStyle: {
                    // e.g. -target x86_64-linux-gnu, -isysroot /path
                    result.query_args.push_back(strings.save(arg->getSpelling()).data());
                    for(auto value: arg->getValues()) {
                        result.query_args.push_back(strings.save(value).data());
                    }
                    break;
                }
                default: {
                    // Flags (no value): -nostdinc, -nostdinc++
                    result.query_args.push_back(strings.save(arg->getSpelling()).data());
                    break;
                }
            }
        },
        [](int, int) {
            // Unknown arguments are silently dropped — they can't be
            // reliably parsed, so we skip them rather than corrupting
            // the cache key.
        });

    return result;
}

llvm::ArrayRef<const char*> ToolchainProvider::query_cached(llvm::StringRef file,
                                                            llvm::StringRef directory,
                                                            llvm::ArrayRef<const char*> arguments) {
    auto [key, query_args] = extract_toolchain_flags(file, arguments);
    auto it = toolchain_cache.find(key);
    if(it != toolchain_cache.end()) {
        return it->second;
    }

    LOG_WARN("Toolchain cache miss (spawning process): file={}, cache_size={}, key_len={}",
             file,
             toolchain_cache.size(),
             key.size());

    auto callback = [&](const char* s) -> const char* {
        return strings.save(s).data();
    };
    toolchain::QueryParams params = {file, directory, query_args, callback};
    auto result = toolchain::query_toolchain(params);

    auto [entry, _] = toolchain_cache.try_emplace(std::move(key), std::move(result));
    return entry->second;
}

std::vector<ToolchainQuery>
    ToolchainProvider::get_pending_queries(llvm::ArrayRef<PendingEntry> entries) {
    llvm::StringMap<bool> seen_keys;
    std::vector<ToolchainQuery> queries;

    for(auto& entry: entries) {
        if(entry.arguments.empty()) {
            continue;
        }

        auto [key, query_args] = extract_toolchain_flags(entry.file, entry.arguments);

        // Skip if already cached or already queued.
        if(toolchain_cache.count(key) || !seen_keys.try_emplace(key, true).second) {
            continue;
        }

        LOG_DEBUG("Pre-warm: new toolchain key (len={}) for file={}", key.size(), entry.file);
        queries.push_back(
            {std::move(key), std::move(query_args), entry.file.str(), entry.directory.str()});
    }

    LOG_INFO("Pre-warm: {} unique keys from {} entries, {} queries needed",
             seen_keys.size(),
             entries.size(),
             queries.size());
    return queries;
}

void ToolchainProvider::inject_results(llvm::ArrayRef<ToolchainResult> results) {
    for(auto& result: results) {
        if(toolchain_cache.count(result.key)) {
            continue;
        }
        std::vector<const char*> saved;
        saved.reserve(result.cc1_args.size());
        for(auto& arg: result.cc1_args) {
            saved.push_back(strings.save(arg).data());
        }
        toolchain_cache.try_emplace(result.key, std::move(saved));
    }
}

bool ToolchainProvider::has_cached_entries() const {
    return !toolchain_cache.empty();
}

}  // namespace clice
