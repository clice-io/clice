module;

#include "modules/prelude.h"

module clice:syntax.header_context;

import :syntax.include_resolver;
import :syntax.scan;

namespace clice {

/// One file along an include chain, from the host source file down to the
/// direct includer of the target header. The target itself is not part of
/// the chain — its buffer is the compile's focus file.
struct ChainEntry {
    /// Absolute path, used for #line markers and include resolution.
    llvm::StringRef path;

    /// File content as read from disk.
    llvm::StringRef content;

    /// The 1-based line of the directive entering the next file, when the
    /// host's include tree tells; 0 picks among the directives naming it.
    std::uint32_t line = 0;
};

/// Resolve an include directive of a file in `includer_dir`, which was
/// itself found in search directory `includer_found_dir`; nullopt when
/// it resolves nowhere.
using IncludeResolver =
    llvm::function_ref<std::optional<ResolveResult>(const ScanResult::IncludeInfo& include,
                                                    llvm::StringRef includer_dir,
                                                    std::optional<unsigned> includer_found_dir)>;

/// A run of a synthesized file copied from the file it was cut from: where
/// it starts in each, and its length.
struct SourceRun {
    std::uint32_t offset = 0;
    std::uint32_t source_offset = 0;
    std::uint32_t length = 0;
};

/// What a synthesized file's text was cut from.
struct SynthesizedOrigin {
    /// The file, and the runs of it the text copies: what an identity
    /// names a position in the text by, so that it agrees with the compile
    /// of that file.
    std::string source;
    std::vector<SourceRun> runs;
};

/// A file a compile reads from memory instead of disk: a fragment of a
/// header context, or the header's snapshot.
struct SynthesizedFile {
    /// How a compile enters the file.
    enum class Entry : std::uint8_t {
        /// Through an include in another synthesized file or on the command
        /// line.
        Included,
        /// Ahead of the focus file, after the command's own -includes, by
        /// every compile but the preamble's build: SynthesizedContext::open.
        Forced,
        /// Through an include appended to the focus file's text, by every
        /// compile serving that text from memory but the preamble's build:
        /// the suffix.
        Appended,
    };

    std::string path;
    std::string content;
    SynthesizedOrigin origin;
    Entry entry = Entry::Included;
};

using SynthesizedFiles = std::vector<SynthesizedFile>;

/// The includer context of a header, as files the compile reads from
/// memory: each chain file cut at its include of the next, the part
/// before the cut and the part after it each becoming one fragment that
/// sits in the chain file's own directory — so its includes resolve there
/// as they do in that file — and ends by including the next fragment.
///
/// The part after the cut, the suffix, closes the braces the header sits
/// in (an X-macro list in an enum or a function body, members in a class
/// body): of the first chain file cut inside braces, the rest up to the
/// `}` closing them, with the declarators up to a `;` after it, or up to
/// the `#endif` of a block they close in; of every later one, which those
/// braces enclose, the whole rest; of a chain file cut outside every
/// brace, nothing. A class body thus goes on to its end, as its member
/// functions read members declared after them. Braces count lexically,
/// both branches of a conditional: a brace a macro writes is not counted,
/// so a closing one never closes and the suffix takes the rest of the
/// file, while a class a macro opens ends at the header's text. The focus
/// file always includes the suffix, if only an empty one. Each fragment
/// starts with a #line marker; a cut inside #if blocks opens matching
/// `#if 1`s, and a suffix fragment ending inside #if blocks closes them.
/// Only the focus file's include enters the suffix: where another file
/// enters the header (an earlier occurrence), the header is its text
/// alone.
struct SynthesizedContext {
    /// The fragment of the host before its cut, entering every later one:
    /// injected via -include. Restores the preprocessor state the header
    /// sees inside the host's translation unit. Empty for an empty chain.
    std::string prefix;

    /// The part of the prefix inside the braces the header sits in, split
    /// off from the last place the chain stood outside every brace: a
    /// precompiled preamble cannot end in a declaration, so the prefix up
    /// to there is the preamble and this file the compile enters after it.
    /// Empty when the header sits outside every brace.
    std::string open;

    /// Every synthesized file: the fragments, and the header's snapshot
    /// when one names it.
    SynthesizedFiles files;

    /// Whether a fragment names the header's snapshot: a chain file
    /// includes the header besides the cut, so the context embeds the
    /// header's disk content.
    bool snapshot = false;
};

/// Synthesize both sides of the includer context of `target_path`.
///
/// For each file in the chain, scans its include directives and finds the
/// one on the entry's line, or else the one that resolves to the next file
/// in the chain (the target for the last entry). The host's own include
/// directives resolve from no search directory; every later chain file is
/// found where its includer's directive resolved, which is where its own
/// `#include_next` resumes. Returns nullopt when a chain step cannot be
/// matched.
///
/// `occurrence` selects among multiple includes of the target in a direct
/// includer (the last chain entry) without a line: a file without include guards
/// can be included several times with different preprocessor states, and
/// each occurrence is a distinct context. It indexes the candidate list in
/// directive order (0-based); out of range fails the synthesis. When
/// unset, unconditional candidates are preferred over ones inside #if
/// blocks.
///
/// `target_content`: the header's disk content, snapshotted next to it
/// for includes of the header itself (other occurrences along the chain).
/// At compile time the target's path is remapped to the open buffer with
/// the suffix's include appended, so keeping such directives verbatim
/// would recurse; they are redirected to the snapshot instead, or blanked
/// (line count preserved) without one or when the header guards itself
/// with `#pragma once`, which the compile enters once.
std::optional<SynthesizedContext>
    synthesize_context(llvm::ArrayRef<ChainEntry> chain,
                       llvm::StringRef target_path,
                       IncludeResolver resolve,
                       std::optional<std::uint32_t> occurrence = {},
                       std::optional<llvm::StringRef> target_content = {});

/// Append an include of `path` to `text` on a line of its own.
void append_include(std::string& text, llvm::StringRef path);

}  // namespace clice
