#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// One file along an include chain, from the host source file down to the
/// direct includer of the target header. The target itself is not part of
/// the chain — its buffer is the compilation main file.
struct ChainEntry {
    /// Absolute path, used for #line markers and include resolution.
    llvm::StringRef path;

    /// File content as read from disk.
    llvm::StringRef content;
};

/// Resolve an include directive to an absolute path.
/// Arguments: raw header name (without delimiters), is_angled,
/// is_include_next, directory of the including file.
/// Returns the resolved absolute path, or nullopt if not found.
using IncludeResolver =
    llvm::function_ref<std::optional<std::string>(llvm::StringRef, bool, bool, llvm::StringRef)>;

/// Synthesize the prefix preamble for a header context.
///
/// For each file in the chain, scans its include directives, finds the one
/// that resolves to the next file in the chain (the target for the last
/// entry), and emits everything before that directive prefixed with a
/// #line marker. The result restores the preprocessor state the target
/// header would see when compiled as part of the host translation unit.
///
/// Matching prefers exact resolved-path equality. If no directive resolves
/// to the next path (e.g. resolution failed for an exotic search setup),
/// falls back to a filename match — but only if it is unambiguous.
/// Returns nullopt when a chain step cannot be matched.
///
/// `occurrence` selects among multiple includes of the target in its
/// direct includer (the last chain entry): a file without include guards
/// can be included several times with different preprocessor states, and
/// each occurrence is a distinct context. It indexes the candidate list in
/// directive order (0-based); out of range fails the synthesis. When
/// unset, unconditional candidates are preferred over ones inside #if
/// blocks.
std::optional<std::string> synthesize_preamble(llvm::ArrayRef<ChainEntry> chain,
                                               llvm::StringRef target_path,
                                               IncludeResolver resolve,
                                               std::optional<std::uint32_t> occurrence = {});

/// Count how many include directives in `content` bring in `target_path`
/// (candidates in the sense of synthesize_preamble's matching).
std::uint32_t count_include_occurrences(llvm::StringRef content,
                                        llvm::StringRef includer_path,
                                        llvm::StringRef target_path,
                                        IncludeResolver resolve);

}  // namespace clice
