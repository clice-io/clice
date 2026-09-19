#pragma once

/// Name matching for symbol search and completion: how identifiers split
/// into words, the matcher that scores a typed pattern against a name,
/// and the tokens a search index keys names by. The three are one
/// design — a token is a path the matcher may take through a name — so a
/// name the matcher accepts always carries every token of the pattern,
/// which is what lets an index answer a fuzzy query by intersecting
/// posting lists (fuzzy_matcher_tests pins this as a property).

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A character's place in its identifier. `XMLHttpRequest_Async` reads
/// `HTTHTTTHTTTTTTSHTTTT`: a head starts a word, a tail continues it, a
/// separator (punctuation, space) lies between words.
enum class CharRole : std::uint8_t {
    Separator,
    Head,
    Tail,
};

/// Split `text` into words. A word begins at the start, after a
/// separator, at an uppercase letter following a lowercase one (`fooBar`)
/// and at the last uppercase letter of a run that lowercase follows (the
/// `R` of `XMLRequest`). Digits and non-ASCII bytes count as lowercase.
/// `roles` has `text`'s size.
void segment(llvm::StringRef text, llvm::MutableArrayRef<CharRole> roles);

/// Matches one pattern against many names, scoring each.
///
/// Every pattern character matches a name character in order, letters
/// case-insensitively. A character lands right after the previous match,
/// on a word head, on a separator, or on an uppercase letter of an
/// initialism (`HTML` in `HTMLElement`); the first may also land inside a
/// word when the options allow it. So `up` finds `unique_ptr`, `print`
/// finds `vsprintf` for a search, and `ob` never finds `foo_bar`. The
/// score rewards heads, contiguous runs and matching case, and penalizes
/// skipped words and a start inside a word, so `foo` ranks `foo` above
/// `foobar` above `bar_foo` above `xfoo`.
struct MatchOptions {
    /// Also accept a name one edit away from the pattern — a substituted,
    /// missing or extra character — at half the score.
    bool typo = false;

    /// Let the first character land inside a word, the run then staying
    /// contiguous to that word's end: `printf` finds `vsprintf`. Off, a
    /// match starts on a word head like it continues after a gap — the
    /// completion filter's choice, where `tru` must not offer `struct`.
    bool inside_word = false;
};

class FuzzyMatcher {
public:
    explicit FuzzyMatcher(llvm::StringRef pattern, MatchOptions options = {});

    /// The score of `name` against the pattern, none when it does not
    /// match: in [0, 1] for a partial match — 1 for a prefix — and up to
    /// 2 when the pattern spells the whole name. An empty pattern scores
    /// every name 1.
    std::optional<float> match(llvm::StringRef name);

    /// `name` with the characters its best match uses bracketed
    /// (`[u]nique[_p]tr`), or empty when it does not match. For tests
    /// and debugging.
    std::string annotate(llvm::StringRef name);

    llvm::StringRef pattern() const {
        return {pat.data(), pat.size()};
    }

    bool empty() const {
        return pat.empty();
    }

private:
    /// How a state was reached, for reconstructing the match.
    enum class Step : std::uint8_t {
        None,
        /// The name character before this state was skipped.
        Skip,
        /// The pattern and name characters before this state match.
        Match,
        /// The pattern character before this state has no counterpart.
        Drop,
        /// The pattern and name characters before this state differ.
        Replace,
    };

    /// What the match is doing at a state: between runs, in a run that
    /// may jump on to a head, or in a run begun inside a word, which must
    /// stay contiguous until that word ends.
    enum class Run : std::uint8_t {
        Gap,
        Free,
        Inside,
    };

    /// The best partial match consuming `p` pattern and `n` name
    /// characters, per run state and whether the typo allowance is spent.
    struct Cell {
        std::int16_t score;
        Step step;
        Run prev_run;
        bool prev_typo;
    };

    Cell& cell(std::size_t p, std::size_t n, Run run, bool typo);
    bool prepare(llvm::StringRef name);
    void fill();
    std::optional<std::pair<int, Cell*>> best();
    llvm::SmallVector<std::uint8_t, 32> matched_positions();

    MatchOptions options;
    llvm::SmallVector<char, 32> pat;
    llvm::SmallVector<char, 32> low_pat;
    bool pat_has_upper = false;
    /// Whether the pattern and the name fit the bounds; past them a
    /// match is partial, never exact.
    bool whole_pattern = true;
    bool whole_name = true;

    llvm::SmallVector<char, 64> name;
    llvm::SmallVector<char, 64> low_name;
    llvm::SmallVector<CharRole, 64> role;
    /// Positions a match may land on after a gap.
    llvm::SmallVector<bool, 64> anchor;

    std::vector<Cell> cells;
};

/// Characters past this many are ignored, by the matcher and by the
/// tokens alike: a longer name is matched on this prefix of it.
constexpr std::size_t name_bound = 127;

/// A token of the search index: one to three lowercase bytes packed
/// big-endian, so the byte count is the width of the value. Tokens below
/// `first_trigram` are the short form of one or two letters.
using NameToken = std::uint32_t;

constexpr NameToken first_trigram = 1u << 16;

/// The tokens under which a search index files a name: every trigram of a
/// path the matcher may take through it, and — for queries too short to
/// form a trigram — the unigrams and bigrams starting at its first two
/// word heads. Sorted, unique.
void name_tokens(llvm::StringRef name, llvm::SmallVectorImpl<NameToken>& out);

/// The tokens every name the matcher accepts for `pattern` carries: the
/// trigrams of its letters in sequence, or for fewer than three letters
/// the one unigram or bigram they form. Empty when the pattern has no
/// letters. Sorted, unique.
void query_tokens(llvm::StringRef pattern, llvm::SmallVectorImpl<NameToken>& out);

/// The tokens a name carrying `pattern` with one character wrong still
/// has: for each position, the trigrams entirely before it and entirely
/// after it as one alternative. An index unions the alternatives'
/// posting-list intersections. Empty for patterns under six letters,
/// where an edit leaves too little to key on.
struct TypoAlternative {
    llvm::SmallVector<NameToken, 8> tokens;
};

void typo_tokens(llvm::StringRef pattern, llvm::SmallVectorImpl<TypoAlternative>& out);

}  // namespace clice
