#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

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

/// Return module names matching a prefix, suitable for `import` completion.
/// @param modules  Module name map (path_id → module name).
/// @param prefix   Partially-typed module name to match against.
std::vector<std::string>
    complete_module_import(const llvm::DenseMap<std::uint32_t, std::string>& modules,
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
