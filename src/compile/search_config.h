#pragma once

#include <string>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

struct SearchDir {
    std::string path;
};

/// Header search configuration extracted from compilation arguments.
/// Uses a three-segment model matching clang's InitHeaderSearch::Realize layout:
///   [Quoted... | Angled... | System...]
///              ^            ^
///      angled_start_idx   system_start_idx
struct SearchConfig {
    /// Ordered list of search directories, partitioned into three segments.
    std::vector<SearchDir> dirs;

    /// Index in dirs where Angled (-I) dirs start.
    /// Quoted ("") includes search from index 0; angled (<>) from here.
    unsigned angled_start_idx = 0;

    /// Index in dirs where System (-isystem, -internal-isystem, etc.) dirs start.
    unsigned system_start_idx = 0;
};

/// Extract header search configuration from compilation arguments.
///
/// Parses user-level flags (-I, -isystem, -iquote) and cc1-level flags
/// (-internal-isystem, -internal-externc-isystem) using the clang argument
/// parser. Relative paths are resolved against the given working directory
/// and normalized with remove_dots().
///
/// This is intentionally a standalone function (not tied to CompilationDatabase)
/// so it can be tested and improved independently to match clang's behavior.
SearchConfig extract_search_config(llvm::ArrayRef<const char*> arguments,
                                    llvm::StringRef directory);

}  // namespace clice
