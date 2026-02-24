#pragma once

#include <optional>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clice {

// Utilities for word segmentation.
// FuzzyMatcher already incorporates this logic, so most users don't need this.
//
// A name like "fooBar_baz" consists of several parts foo, bar, baz.
// Aligning segmentation of word and pattern improves the fuzzy-match.
// For example: [lol] matches "LaughingOutLoud" better than "LionPopulation"
//
// First we classify each character into types (uppercase, lowercase, etc).
// Then we look at the sequence: e.g. [upper, lower] is the start of a segment.

// We distinguish the types of characters that affect segmentation.
// It's not obvious how to segment digits, we treat them as lowercase letters.
// As we don't decode UTF-8, we treat bytes over 127 as lowercase too.
// This means we require exact (case-sensitive) match for those characters.
enum CharType : unsigned char {
    Empty = 0,        // Before-the-start and after-the-end (and control chars).
    Lower = 1,        // Lowercase letters, digits, and non-ASCII bytes.
    Upper = 2,        // Uppercase letters.
    Punctuation = 3,  // ASCII punctuation (including Space)
};

// A CharTypeSet is a bitfield representing all the character types in a word.
// Its bits are 1<<Empty, 1<<Lower, etc.
using CharTypeSet = unsigned char;

// Each character's Role is the Head or Tail of a segment, or a Separator.
// e.g. XMLHttpRequest_Async
//      +--+---+------ +----
//      ^Head   ^Tail ^Separator
enum CharRole : unsigned char {
    Unknown = 0,    // Stray control characters or impossible states.
    Tail = 1,       // Part of a word segment, but not the first character.
    Head = 2,       // The first character of a word segment.
    Separator = 3,  // Punctuation characters that separate word segments.
};

// Compute segmentation of Text.
// Character roles are stored in Roles (Roles.size() must equal Text.size()).
// The set of character types encountered is returned, this may inform
// heuristics for dealing with poorly-segmented identifiers like "strndup".
CharTypeSet calculate_roles(llvm::StringRef Text, llvm::MutableArrayRef<CharRole> Roles);

// A matcher capable of matching and scoring strings against a single pattern.
// It's optimized for matching against many strings - match() does not allocate.
class FuzzyMatcher {
public:
    // Characters beyond MaxPat are ignored.
    FuzzyMatcher(llvm::StringRef Pattern);

    // If Word matches the pattern, return a score indicating the quality match.
    // Scores usually fall in a [0,1] range, with 1 being a very good score.
    // "Super" scores in (1,2] are possible if the pattern is the full word.
    // Characters beyond MaxWord are ignored.
    std::optional<float> match(llvm::StringRef Word);

    llvm::StringRef pattern() const {
        return llvm::StringRef(Pat, pat_n);
    }

    bool empty() const {
        return pat_n == 0;
    }

    // Dump internal state from the last match() to the stream, for debugging.
    // Returns the pattern with [] around matched characters, e.g.
    //   [u_p] + "unique_ptr" --> "[u]nique[_p]tr"
    llvm::SmallString<256> dumpLast(llvm::raw_ostream&) const;

private:
    // We truncate the pattern and the word to bound the cost of matching.
    constexpr inline static int MaxPat = 63, MaxWord = 127;
    // Action describes how a word character was matched to the pattern.
    // It should be an enum, but this causes bitfield problems:
    //   - for MSVC the enum type must be explicitly unsigned for correctness
    //   - GCC 4.8 complains not all values fit if the type is unsigned
    using Action = bool;
    constexpr static Action Miss = false;  // Word character was skipped.
    constexpr static Action Match = true;  // Matched against a pattern character.

    bool init(llvm::StringRef Word);
    void build_graph();
    bool allow_match(int P, int W, Action Last) const;
    int skip_penalty(int W, Action Last) const;
    int match_bonus(int P, int W, Action Last) const;

    // Pattern data is initialized by the constructor, then constant.
    char Pat[MaxPat];           // Pattern data
    int pat_n;                  // Length
    char low_pat[MaxPat];       // Pattern in lowercase
    CharRole pat_role[MaxPat];  // Pattern segmentation info
    CharTypeSet pat_type_set;   // Bitmask of 1<<CharType for all Pattern characters
    float score_scale;          // Normalizes scores for the pattern length.

    // Word data is initialized on each call to match(), mostly by init().
    char word[MaxWord];           // Word data
    int word_n;                   // Length
    char low_word[MaxWord];       // Word in lowercase
    CharRole word_role[MaxWord];  // Word segmentation info
    CharTypeSet word_type_set;    // Bitmask of 1<<CharType for all Word characters
    bool word_contains_pattern;   // Simple substring check

    // Cumulative best-match score table.
    // Boundary conditions are filled in by the constructor.
    // The rest is repopulated for each match(), by buildGraph().
    struct ScoreInfo {
        signed int score : 15;
        Action Prev : 1;
    };

    ScoreInfo scores[MaxPat + 1][MaxWord + 1][/* Last Action */ 2];
};

}  // namespace clice
