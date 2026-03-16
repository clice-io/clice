#include "server/cache_manager.h"

#include "support/logging.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

void CacheManager::initialize(const Config& config, llvm::StringRef workspace) {
    if(!config.project.cache_dir.empty()) {
        cache_dir_ = config.project.cache_dir;
    } else {
        llvm::SmallString<256> path(workspace);
        llvm::sys::path::append(path, ".clice", "cache");
        cache_dir_ = std::string(path);
    }

    max_size_ = 2ULL * 1024 * 1024 * 1024;

    llvm::sys::fs::create_directories(cache_dir_);
    LOG_INFO("Cache directory: {}", cache_dir_);
}

static std::string hash_to_hex(uint64_t hash) {
    llvm::SmallString<16> hex;
    llvm::raw_svector_ostream os(hex);
    os << llvm::format_hex_no_prefix(hash, 16);
    return std::string(hex);
}

std::string CacheManager::pch_path(llvm::StringRef source_file) const {
    auto hash = llvm::xxh3_64bits(source_file);
    llvm::SmallString<256> path(cache_dir_);
    llvm::sys::path::append(path, "pch");
    llvm::sys::fs::create_directories(path);

    std::string filename = hash_to_hex(hash) + ".pch";
    llvm::sys::path::append(path, filename);
    return std::string(path);
}

std::string CacheManager::pcm_path(llvm::StringRef module_name) const {
    auto hash = llvm::xxh3_64bits(module_name);
    llvm::SmallString<256> path(cache_dir_);
    llvm::sys::path::append(path, "pcm");
    llvm::sys::fs::create_directories(path);

    std::string filename = hash_to_hex(hash) + ".pcm";
    llvm::sys::path::append(path, filename);
    return std::string(path);
}

bool CacheManager::exists(llvm::StringRef path) const {
    return llvm::sys::fs::exists(path);
}

void CacheManager::evict_if_over_limit() {
    std::error_code ec;
    std::uint64_t total_size = 0;

    struct CacheEntry {
        std::string path;
        llvm::sys::TimePoint<> access_time;
        std::uint64_t size;
    };
    llvm::SmallVector<CacheEntry> entries;

    for(auto it = llvm::sys::fs::recursive_directory_iterator(cache_dir_, ec);
        it != llvm::sys::fs::recursive_directory_iterator(); it.increment(ec)) {
        if(ec)
            break;

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(it->path(), status))
            continue;
        if(status.type() != llvm::sys::fs::file_type::regular_file)
            continue;

        auto size = status.getSize();
        total_size += size;
        entries.push_back({std::string(it->path()),
                           status.getLastAccessedTime(),
                           size});
    }

    if(max_size_ == 0 || total_size <= max_size_)
        return;

    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.access_time < b.access_time;
              });

    for(auto& entry : entries) {
        if(total_size <= max_size_)
            break;
        llvm::sys::fs::remove(entry.path);
        total_size -= entry.size;
        LOG_INFO("Evicted cache file: {}", entry.path);
    }
}

}  // namespace clice
