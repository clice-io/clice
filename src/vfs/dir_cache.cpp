module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :support.logging;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.path;

namespace clice::vfs {

namespace {

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool case_insensitive = true;
#else
constexpr bool case_insensitive = false;
#endif

llvm::SmallString<64> fold(llvm::StringRef name) {
    llvm::SmallString<64> folded;
    for(char c: name) {
        folded.push_back(llvm::toLower(c));
    }
    return folded;
}

/// Whether the entry `name` opens in `listing` is a directory; nullopt
/// when it opens none.
std::optional<bool> entry_is_directory(const Listing& listing, llvm::StringRef name) {
    if(auto it = listing.entries.find(name); it != listing.entries.end()) {
        return it->second;
    }
    if constexpr(!case_insensitive) {
        return std::nullopt;
    }
    if(llvm::isASCII(name) && !listing.folded.contains(fold(name))) {
        return std::nullopt;
    }
    llvm::SmallString<256> entry(listing.dir);
    path::push(entry, name);
    auto found = fs::status(entry.str());
    if(!found) {
        return std::nullopt;
    }
    return found->type == fs::FileType::Directory;
}

}  // namespace

bool Listing::contains(llvm::StringRef name) const {
    return entry_is_directory(*this, name).has_value();
}

bool Listing::contains_file(llvm::StringRef name) const {
    auto directory = entry_is_directory(*this, name);
    return directory && !*directory;
}

std::shared_ptr<const Listing> list(llvm::StringRef dir) {
    auto listing = std::make_shared<Listing>();
    listing->dir = dir.str();
    auto before = fs::status(dir);
    auto entries = fs::Directory::open(dir);
    if(!entries) {
        LOG_DEBUG("readdir failed for '{}': {}", dir, entries.error().message());
        return listing;
    }
    for(auto& entry: *entries) {
        if(!entry) {
            LOG_DEBUG("readdir failed for '{}': {}", dir, entry.error().message());
            return listing;
        }
        bool directory = entry->type == fs::FileType::Directory;
        if(entry->type == fs::FileType::Symlink) {
            directory = fs::is_directory(path::join(dir, entry->name));
        }
        listing->entries.try_emplace(entry->name, directory);
        if constexpr(case_insensitive) {
            listing->folded.insert(fold(entry->name));
        }
    }

    if(!before) {
        return listing;
    }
    auto after = fs::status(dir);
    if(after && before->mtime == after->mtime) {
        auto mtime_ns = Stamp::of(*after).mtime_ns;
        if(stable_mtime(mtime_ns)) {
            listing->mtime_ns = mtime_ns;
        }
    }
    return listing;
}

std::shared_ptr<const Listing> DirCache::kept(llvm::StringRef dir) const {
    auto it = listings.find(dir);
    if(it == listings.end() || it->second->mtime_ns == 0) {
        return nullptr;
    }
    return it->second;
}

const Listing& Scope::list(llvm::StringRef dir) {
    if(auto it = held.find(dir); it != held.end()) {
        return *it->second;
    }

    if(auto kept = cache.kept(dir)) {
        if(auto status = fs::status(dir); status && Stamp::of(*status).mtime_ns == kept->mtime_ns) {
            return *held.try_emplace(dir, std::move(kept)).first->second;
        }
    }

    auto listing = vfs::list(dir);
    adopt(dir, listing);
    return *listing;
}

void Scope::adopt(llvm::StringRef dir, std::shared_ptr<const Listing> listing) {
    cache.listings[dir] = listing;
    held[dir] = std::move(listing);
}

}  // namespace clice::vfs
