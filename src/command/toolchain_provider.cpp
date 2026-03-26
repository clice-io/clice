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
    /// The key includes all flags except user-content options (-I/-D/-U/etc.),
    /// so the cc1 result reflects the correct compiler semantics (-f/-W/-O/etc.)
    /// and only user-content options need to be replayed after cache lookup.
    llvm::StringMap<std::vector<const char*>> toolchain_cache;

    /// Options excluded from the cache key and toolchain query. These are
    /// per-file user content (include paths, defines, forced includes) or
    /// input files. They don't affect compiler semantics or system path
    /// discovery, and are replayed into the cc1 result afterward.
    static bool is_excluded_option(unsigned id) {
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

    /// Extract flags for the toolchain query. All options except user-content
    /// options (-I/-D/-U/etc.) are included in both the cache key and query args,
    /// so the cc1 result correctly reflects compiler semantics (-f/-W/-O/etc.).
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
                if(is_excluded_option(id)) {
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
                // Unknown arguments are included in key/query to be safe.
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
