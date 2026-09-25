#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

/// Files a compile reads from memory instead of disk: (path, content).
using SynthesizedFiles = std::vector<std::pair<std::string, std::string>>;

/// The includer context of a header, as files the compile reads from
/// memory: each chain file cut at its include of the next, the part
/// before the cut and the part after it each becoming one fragment that
/// sits in the chain file's own directory — so its includes resolve there
/// as they do in that file — and ends by including the next fragment.
struct SynthesizedContext {
    /// The fragment of the host before its cut, entering every later one:
    /// injected via -include. Restores the preprocessor state the header
    /// sees inside the host's translation unit. Empty for an empty chain.
    std::string prefix;

    /// The fragment of the direct includer after its cut, entering the
    /// rest of the chain up to the host: injected by appending one
    /// #include line to the header's buffer, so X-macro fragments
    /// embedded in enums or function bodies see their surrounding braces
    /// close. Each fragment starts with a #line marker; a cut inside #if
    /// blocks opens matching `#if 1`s so the fragment's own #endifs stay
    /// balanced. Empty for an empty chain.
    std::string suffix;

    /// Every synthesized file: the fragments, and the header's snapshot
    /// when one was given.
    SynthesizedFiles files;
};

/// Synthesize both sides of the includer context of `target_path`.
///
/// For each file in the chain, scans its include directives and finds the
/// one that resolves to the next file in the chain (the target for the
/// last entry). Matching prefers exact resolved-path equality. If no
/// directive resolves to the next path (e.g. resolution failed for an
/// exotic search setup), falls back to a filename match — but only if it
/// is unambiguous. Returns nullopt when a chain step cannot be matched.
///
/// `occurrence` selects among multiple includes of the target in its
/// direct includer (the last chain entry): a file without include guards
/// can be included several times with different preprocessor states, and
/// each occurrence is a distinct context. It indexes the candidate list in
/// directive order (0-based); out of range fails the synthesis. When
/// unset, unconditional candidates are preferred over ones inside #if
/// blocks.
///
/// `target_content`: the header's disk content, snapshotted next to it
/// for includes of the header itself (other occurrences along the chain).
/// At compile time the target's path is remapped to the open buffer with
/// a trailing suffix include, so keeping such directives verbatim would
/// recurse; they are redirected to the snapshot instead, or blanked (line
/// count preserved) without one.
std::optional<SynthesizedContext>
    synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                       llvm::StringRef target_path,
                       IncludeResolver resolve,
                       std::optional<std::uint32_t> occurrence = {},
                       std::optional<llvm::StringRef> target_content = {});

/// Count how many include directives in `content` bring in `target_path`
/// (candidates in the sense of synthesize_context's matching).
std::uint32_t count_include_occurrences(llvm::StringRef content,
                                        llvm::StringRef includer_path,
                                        llvm::StringRef target_path,
                                        IncludeResolver resolve);

}  // namespace clice
