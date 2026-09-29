#include "vfs/dir_cache.h"

#include <chrono>

#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/Path.h"

namespace clice::vfs {

std::shared_ptr<const Listing> list(llvm::StringRef dir) {
    auto listing = std::make_shared<Listing>();
    llvm::sys::fs::file_status before;
    bool have_before = !llvm::sys::fs::status(dir, before);
    std::error_code ec;
    llvm::sys::fs::directory_iterator it(dir, ec);
    if(ec) {
        LOG_DEBUG("readdir failed for '{}': {}", dir, ec.message());
    }
    for(; !ec && it != llvm::sys::fs::directory_iterator(); it.increment(ec)) {
        auto type = it->type();
        bool directory = type == llvm::sys::fs::file_type::directory_file;
        if(type == llvm::sys::fs::file_type::symlink_file ||
           type == llvm::sys::fs::file_type::type_unknown) {
            directory = llvm::sys::fs::is_directory(it->path());
        }
        listing->entries.try_emplace(llvm::sys::path::filename(it->path()), directory);
    }

    llvm::sys::fs::file_status after;
    if(have_before && !ec && !llvm::sys::fs::status(dir, after) &&
       fs::mtime_ns(before) == fs::mtime_ns(after) && fs::settled(fs::mtime_ns(after))) {
        listing->mtime_ns = fs::mtime_ns(after);
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
        llvm::sys::fs::file_status status;
        if(!llvm::sys::fs::status(dir, status) && fs::mtime_ns(status) == kept->mtime_ns) {
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
