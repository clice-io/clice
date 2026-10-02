#include "vfs/dir_cache.h"

#include <chrono>

#include "support/logging.h"
#include "vfs/file_system.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"

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

}  // namespace

bool Listing::contains(llvm::StringRef name) const {
    if(entries.contains(name)) {
        return true;
    }
    if constexpr(!case_insensitive) {
        return false;
    }
    if(llvm::isASCII(name) && !folded.contains(fold(name))) {
        return false;
    }
    llvm::SmallString<256> path(dir);
    llvm::sys::path::append(path, name);
    return vfs::exists(path);
}

std::shared_ptr<const Listing> list(llvm::StringRef dir) {
    auto listing = std::make_shared<Listing>();
    listing->dir = dir.str();
    auto before = vfs::status(dir);
    auto entries = read_dir(dir);
    if(!entries) {
        LOG_DEBUG("readdir failed for '{}': {}", dir, entries.error().message());
        return listing;
    }
    for(auto& entry: *entries) {
        bool directory = entry.type == llvm::sys::fs::file_type::directory_file;
        if(entry.type == llvm::sys::fs::file_type::symlink_file) {
            directory = vfs::is_directory(entry.path);
        }
        auto name = llvm::sys::path::filename(entry.path);
        listing->entries.try_emplace(name, directory);
        if constexpr(case_insensitive) {
            listing->folded.insert(fold(name));
        }
    }

    if(!before) {
        return listing;
    }
    auto after = vfs::status(dir);
    if(after && before->stamp.mtime_ns == after->stamp.mtime_ns && settled(after->stamp.mtime_ns)) {
        listing->mtime_ns = after->stamp.mtime_ns;
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
        stats.reused += 1;
        return *it->second;
    }

    if(auto kept = cache.kept(dir)) {
        if(auto status = vfs::status(dir); status && status->stamp.mtime_ns == kept->mtime_ns) {
            stats.reused += 1;
            return *held.try_emplace(dir, std::move(kept)).first->second;
        }
    }

    stats.listed += 1;
    auto start = std::chrono::steady_clock::now();
    auto listing = vfs::list(dir);
    stats.us += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count();
    adopt(dir, listing);
    return *listing;
}

void Scope::adopt(llvm::StringRef dir, std::shared_ptr<const Listing> listing) {
    cache.listings[dir] = listing;
    held[dir] = std::move(listing);
}

}  // namespace clice::vfs
