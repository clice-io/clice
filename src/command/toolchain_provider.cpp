#include "command/toolchain_provider.h"

#include "command/driver.h"
#include "command/toolchain.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/object_pool.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

namespace clice {

using ID = clang::driver::options::ID;

struct ToolchainProvider::Impl {
    llvm::BumpPtrAllocator allocator;
    StringSet strings{allocator};
    ArgumentParser parser{&allocator};

    /// Cache of toolchain query results, keyed by canonical toolchain key.
    /// The key captures only flags that affect system path discovery (driver,
    /// target, sysroot, stdlib, etc.), so files sharing the same compiler
    /// configuration share one cached result.
    llvm::StringMap<std::vector<const char*>> toolchain_cache;

    /// Option IDs that affect system path discovery. These determine the
    /// toolchain cache key and are the only flags passed to the toolchain query.
    static bool is_toolchain_option(unsigned id) {
        switch(id) {
            case ID::OPT_target:
            case ID::OPT_target_legacy_spelling:
            case ID::OPT_isysroot:
            case ID::OPT__sysroot_EQ:
            case ID::OPT__sysroot:
            case ID::OPT_stdlib_EQ:
            case ID::OPT_gcc_toolchain:
            case ID::OPT_gcc_install_dir_EQ:
            case ID::OPT_nostdinc:
            case ID::OPT_nostdincxx:
            case ID::OPT_std_EQ:
            case ID::OPT_x: return true;
            default: return false;
        }
    }

    /// Extract toolchain-relevant flags from arguments using the clang argument
    /// parser. Returns both a cache key string and a minimal argument list for
    /// the toolchain query. Using the parser ensures all flag forms (joined,
    /// separate, etc.) are handled correctly.
    struct ToolchainExtract {
        std::string key;
        std::vector<const char*> query_args;
    };

    ToolchainExtract extract_toolchain_flags(this Impl& self,
                                             llvm::StringRef file,
                                             llvm::ArrayRef<const char*> arguments) {
        ToolchainExtract result;

        // Driver binary (first arg) — e.g. "clang++" vs "clang" affects language mode.
        result.key += arguments[0];
        result.key += '\0';

        // File extension affects language mode (C vs C++).
        result.key += path::extension(file);
        result.key += '\0';

        result.query_args.push_back(arguments[0]);

        self.parser.parse(
            llvm::ArrayRef(arguments).drop_front(),
            [&](std::unique_ptr<llvm::opt::Arg> arg) {
                auto id = arg->getOption().getID();
                if(!is_toolchain_option(id)) {
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
                        result.query_args.push_back(self.strings.save(joined).data());
                        break;
                    }
                    case llvm::opt::Option::RenderSeparateStyle: {
                        // e.g. -target x86_64-linux-gnu, -isysroot /path
                        result.query_args.push_back(self.strings.save(arg->getSpelling()).data());
                        for(auto value: arg->getValues()) {
                            result.query_args.push_back(self.strings.save(value).data());
                        }
                        break;
                    }
                    default: {
                        // Flags (no value): -nostdinc, -nostdinc++
                        result.query_args.push_back(self.strings.save(arg->getSpelling()).data());
                        break;
                    }
                }
            },
            [](int, int) {
                // Ignore unknown arguments — they won't affect toolchain discovery.
            });

        return result;
    }

    /// Query toolchain with caching. Returns the cached cc1 args for the given
    /// toolchain key, running the expensive query only on cache miss.
    llvm::ArrayRef<const char*> query_toolchain_cached(this Impl& self,
                                                       llvm::StringRef file,
                                                       llvm::StringRef directory,
                                                       llvm::ArrayRef<const char*> arguments) {
        auto [key, query_args] = self.extract_toolchain_flags(file, arguments);
        auto it = self.toolchain_cache.find(key);
        if(it != self.toolchain_cache.end()) {
            return it->second;
        }

        LOG_WARN("Toolchain cache miss (spawning process): file={}, cache_size={}, key_len={}",
                 file,
                 self.toolchain_cache.size(),
                 key.size());

        auto callback = [&](const char* s) -> const char* {
            return self.strings.save(s).data();
        };
        toolchain::QueryParams params = {file, directory, query_args, callback};
        auto result = toolchain::query_toolchain(params);

        auto [entry, _] = self.toolchain_cache.try_emplace(std::move(key), std::move(result));
        return entry->second;
    }
};

ToolchainProvider::ToolchainProvider() : self(std::make_unique<Impl>()) {}

ToolchainProvider::~ToolchainProvider() = default;

ToolchainProvider::ToolchainProvider(ToolchainProvider&&) noexcept = default;

ToolchainProvider& ToolchainProvider::operator=(ToolchainProvider&&) noexcept = default;

llvm::ArrayRef<const char*> ToolchainProvider::query_cached(llvm::StringRef file,
                                                            llvm::StringRef directory,
                                                            llvm::ArrayRef<const char*> arguments) {
    return self->query_toolchain_cached(file, directory, arguments);
}

std::vector<ToolchainQuery>
    ToolchainProvider::get_pending_queries(llvm::ArrayRef<PendingEntry> entries) {
    llvm::StringMap<bool> seen_keys;
    std::vector<ToolchainQuery> queries;

    for(auto& entry: entries) {
        if(entry.arguments.empty()) {
            continue;
        }

        auto [key, query_args] = self->extract_toolchain_flags(entry.file, entry.arguments);

        // Skip if already cached or already queued.
        if(self->toolchain_cache.count(key) || !seen_keys.try_emplace(key, true).second) {
            continue;
        }

        LOG_DEBUG("Pre-warm: new toolchain key (len={}) for file={}", key.size(), entry.file);
        queries.push_back({std::move(key), std::move(query_args), entry.file, entry.directory});
    }

    LOG_INFO("Pre-warm: {} unique keys from {} entries, {} queries needed",
             seen_keys.size(),
             entries.size(),
             queries.size());
    return queries;
}

void ToolchainProvider::inject_results(llvm::ArrayRef<ToolchainResult> results) {
    for(auto& result: results) {
        if(self->toolchain_cache.count(result.key)) {
            continue;
        }
        std::vector<const char*> saved;
        saved.reserve(result.cc1_args.size());
        for(auto& arg: result.cc1_args) {
            saved.push_back(self->strings.save(arg).data());
        }
        self->toolchain_cache.try_emplace(result.key, std::move(saved));
    }
}

bool ToolchainProvider::has_cached_entries() const {
    return !self->toolchain_cache.empty();
}

}  // namespace clice
