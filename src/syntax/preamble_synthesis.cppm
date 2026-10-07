module;

#include "modules/prelude.h"

module clice:syntax.preamble_synthesis;

import :syntax.include_resolver;
import :syntax.scan;

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

/// Resolve an include directive of a file in `includer_dir`, which was
/// itself found in search directory `includer_found_dir`; nullopt when
/// it resolves nowhere.
using IncludeResolver =
    llvm::function_ref<std::optional<ResolveResult>(const ScanResult::IncludeInfo& include,
                                                    llvm::StringRef includer_dir,
                                                    std::optional<unsigned> includer_found_dir)>;

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
    /// when one names it.
    SynthesizedFiles files;

    /// Whether a fragment names the header's snapshot: a chain file
    /// includes the header besides the cut, so the context embeds the
    /// header's disk content.
    bool snapshot = false;

    /// Append the suffix as one trailing #include line: the suffix content
    /// lives in its own synthesized file so features never see it, while
    /// the token stream still closes any braces the fragment is embedded
    /// in. The single extra line sits past the editor's EOF and is
    /// invisible to the client.
    void append_suffix_include(std::string& text) const;
};

/// Synthesize both sides of the includer context of `target_path`.
///
/// For each file in the chain, scans its include directives and finds the
/// one that resolves to the next file in the chain (the target for the
/// last entry). The host is the main file; every later chain file is
/// found where its includer's directive resolved, which is where its own
/// `#include_next` resumes. Returns nullopt when a chain step cannot be
/// matched.
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

}  // namespace clice
