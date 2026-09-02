#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vfs/file_table.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

class DependencyGraph;

struct ResolvedSearchConfig;
struct DirListingCache;

/// What kind of preamble-level completion is being requested.
enum class CompletionContext : std::uint8_t {
    None,
    IncludeQuoted,
    IncludeAngled,
    Import,
};

/// Result of detecting the completion context from source text.
struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;
};

/// Detect whether the cursor is inside a #include or import directive.
/// Pure text parsing — no compiler state needed.
PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset);

/// The names of the graph's provided modules that start with `prefix`,
/// suitable for `import` completion.
std::vector<std::string> complete_module_import(const DependencyGraph& graph,
                                                llvm::StringRef prefix);

/// Entry in the include path completion result.
struct IncludeCandidate {
    std::string name;
    bool is_directory = false;
};

/// Return file/directory names matching a prefix in the given search paths.
/// @param resolved  Pre-resolved search directories with cached directory listings.
/// @param angled_start  Index where angled (<>) search dirs begin.
/// @param prefix    Partially-typed include path (e.g. "vec" or "sys/").
/// @param angled    True for <> includes, false for "" includes.
/// @param dir_cache  Shared directory listing cache (for subdirectory lookups).
std::vector<IncludeCandidate> complete_include_path(const ResolvedSearchConfig& resolved,
                                                    llvm::StringRef prefix,
                                                    bool angled,
                                                    DirListingCache& dir_cache);

}  // namespace clice
