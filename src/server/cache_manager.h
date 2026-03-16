#pragma once

#include <cstddef>
#include <string>

#include "server/config.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

/// Manages on-disk cache for PCH and PCM artifacts.
/// Provides deterministic paths for cache files and handles
/// eviction when the cache exceeds size limits.
class CacheManager {
public:
    void initialize(const Config& config, llvm::StringRef workspace);

    std::string pch_path(llvm::StringRef source_file) const;
    std::string pcm_path(llvm::StringRef module_name) const;

    bool exists(llvm::StringRef path) const;
    void evict_if_over_limit();

    const std::string& cache_dir() const { return cache_dir_; }

private:
    std::string cache_dir_;
    std::size_t max_size_ = 0;
};

}  // namespace clice
