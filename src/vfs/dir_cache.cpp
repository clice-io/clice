module clice;

import :support.logging;
import :vfs.dir_cache;
import :vfs.file_system;
import :vfs.path;

namespace clice::vfs {

llvm::SmallString<64> fold_case(llvm::StringRef name) {
    llvm::SmallString<64> folded;
    for(char c: name) {
        folded.push_back(llvm::toLower(c));
    }
    return folded;
}

namespace {

/// Whether the entry `name` opens in `listing` is a directory; nullopt
/// when it opens none.
std::optional<bool> entry_is_directory(const Listing& listing, llvm::StringRef name) {
    if(auto it = listing.entries.find(name); it != listing.entries.end()) {
        return it->second;
    }
    if(llvm::isASCII(name) &&
       (listing.case_sensitive || !listing.folded.contains(fold_case(name)))) {
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
    auto entries = fs::Directory::open(dir);
    if(!entries) {
        if(entries.error() != std::errc::no_such_file_or_directory) {
            LOG_DEBUG("readdir failed for '{}': {}", dir, entries.error().message());
        }
        return listing;
    }
    auto before = entries->status();
    // Folding where the volume would match case anyway only costs a stat.
    listing->case_sensitive = fs::case_sensitive(dir).value_or(false);
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
        if(!listing->case_sensitive) {
            listing->folded.insert(fold_case(entry->name));
        }
    }

    if(!before) {
        return listing;
    }
    auto after = entries->status();
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

/// The listing of `dir` the cache keeps, when one stat confirms it.
static std::shared_ptr<const Listing> confirmed(const DirCache& cache, llvm::StringRef dir) {
    auto kept = cache.kept(dir);
    if(!kept) {
        return nullptr;
    }
    auto status = fs::status(dir);
    return status && Stamp::of(*status).mtime_ns == kept->mtime_ns ? kept : nullptr;
}

const Listing& Scope::list(llvm::StringRef dir) {
    if(auto it = held.find(dir); it != held.end()) {
        return *it->second;
    }
    if(!operation) {
        if(auto kept = confirmed(cache, dir)) {
            return *held.try_emplace(dir, std::move(kept)).first->second;
        }
        auto listing = vfs::list(dir);
        adopt(dir, listing);
        return *listing;
    }

    if(auto it = operation->held.find(dir); it != operation->held.end()) {
        return *held.try_emplace(dir, it->second).first->second;
    }
    {
        std::lock_guard guard(shared->lock);
        if(auto it = shared->listed.find(dir); it != shared->listed.end()) {
            return *held.try_emplace(dir, it->second).first->second;
        }
    }
    auto listing = confirmed(cache, dir);
    if(!listing) {
        listing = vfs::list(dir);
    }
    std::lock_guard guard(shared->lock);
    auto& first = shared->listed.try_emplace(dir, std::move(listing)).first->second;
    return *held.try_emplace(dir, first).first->second;
}

void Scope::adopt(llvm::StringRef dir, std::shared_ptr<const Listing> listing) {
    if(held.try_emplace(dir, listing).second) {
        cache.listings[dir] = std::move(listing);
    }
}

void Scope::take(SharedListings& shared) {
    for(auto& entry: shared.listed) {
        adopt(entry.getKey(), entry.second);
    }
}

}  // namespace clice::vfs
