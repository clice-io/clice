#pragma once

#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct Arg;

struct SearchDir {
    std::string path;
};

/// Header search configuration extracted from compilation arguments.
/// Uses a four-segment model matching clang's InitHeaderSearch::Realize layout:
///   [Quoted... | Angled... | System... | After...]
///              ^            ^            ^
///      angled_start_idx   system_start_idx  after_start_idx
struct SearchConfig {
    /// Ordered list of search directories, partitioned into four segments.
    std::vector<SearchDir> dirs;

    /// Index in dirs where Angled (-I) dirs start.
    /// Quoted ("") includes search from index 0; angled (<>) from here.
    unsigned angled_start_idx = 0;

    /// Index in dirs where System (-isystem, -internal-isystem, etc.) dirs start.
    unsigned system_start_idx = 0;

    /// Index in dirs where After (-idirafter, -iwithprefix) dirs start.
    unsigned after_start_idx = 0;
};

/// Extract header search configuration from a structured command (driver
/// level or resolved cc1 level — the internal cc1 spellings are covered).
/// Relative paths are resolved against the given working directory and
/// normalized with remove_dots().
SearchConfig extract_search_config(llvm::ArrayRef<Arg> args, llvm::StringRef directory);

}  // namespace clice
