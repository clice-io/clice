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

/// Prefix and suffix restoring the full includer context of a header.
struct SynthesizedContext {
    /// Everything before the target's include, host-first (see
    /// synthesize_preamble). Injected via -include.
    std::string prefix;

    /// Everything after the target's include, mirrored: the rest of the
    /// direct includer first, then the rest of its includer, up to the
    /// host. Each fragment gets a #line marker; a cut inside #if blocks
    /// opens matching `#if 1`s so the fragment's own #endifs stay
    /// balanced. Injected by appending one #include line to the header's
    /// buffer, so X-macro fragments embedded in enums or function bodies
    /// see their surrounding braces close.
    std::string suffix;
};

/// Synthesize both sides of the includer context in one pass. The prefix
/// equals synthesize_preamble's result for the same inputs.
/// `self_snapshot_path`: replacement target for includes of the header
/// itself (other occurrences along the chain). At compile time the
/// target's path is remapped to the open buffer with a trailing suffix
/// include, so keeping such directives verbatim would recurse; they are
/// redirected to a disk snapshot of the header instead, or blanked (line
/// count preserved) when no snapshot is provided.
std::optional<SynthesizedContext> synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                                                     llvm::StringRef target_path,
                                                     IncludeResolver resolve,
                                                     std::optional<std::uint32_t> occurrence = {},
                                                     llvm::StringRef self_snapshot_path = {});

/// Count how many include directives in `content` bring in `target_path`
/// (candidates in the sense of synthesize_preamble's matching).
std::uint32_t count_include_occurrences(llvm::StringRef content,
                                        llvm::StringRef includer_path,
                                        llvm::StringRef target_path,
                                        IncludeResolver resolve);

}  // namespace clice
