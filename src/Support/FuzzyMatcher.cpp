// To check for a match between a Pattern ('u_p') and a Word ('unique_ptr'),
// we consider the possible partial match states:
//
//     u n i q u e _ p t r
//   +---------------------
//   |A . . . . . . . . . .
//  u|
//   |. . . . . . . . . . .
//  _|
//   |. . . . . . . O . . .
//  p|
//   |. . . . . . . . . . B
//
// Each dot represents some prefix of the pattern being matched against some
// prefix of the word.
//   - A is the initial state: '' matched against ''
//   - O is an intermediate state: 'u_' matched against 'unique_'
//   - B is the target state: 'u_p' matched against 'unique_ptr'
//
// We aim to find the best path from A->B.
//  - Moving right (consuming a word character)
//    Always legal: not all word characters must match.
//  - Moving diagonally (consuming both a word and pattern character)
//    Legal if the characters match.
//  - Moving down (consuming a pattern character) is never legal.
//    Never legal: all pattern characters must match something.
// Characters are matched case-insensitively.
// The first pattern character may only match the start of a word segment.
//
// The scoring is based on heuristics:
//  - when matching a character, apply a bonus or penalty depending on the
//    match quality (does case match, do word segments align, etc)
//  - when skipping a character, apply a penalty if it hurts the match
//    (it starts a word segment, or splits the matched region, etc)
//
// These heuristics require the ability to "look backward" one character, to
// see whether it was matched or not. Therefore the dynamic-programming matrix
// has an extra dimension (last character matched).
// Each entry also has an additional flag indicating whether the last-but-one
// character matched, which is needed to trace back through the scoring table
// and reconstruct the match.
//
// We treat strings as byte-sequences, so only ASCII has first-class support.
//
// This algorithm was inspired by VS code's client-side filtering, and aims
// to be mostly-compatible.
//
//===----------------------------------------------------------------------===//

#include "Support/FuzzyMatcher.h"

#include "llvm/Support/Format.h"

namespace clice {

static char lower(char C) {
    return C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C;
}

// A "negative infinity" score that won't overflow.
// We use this to mark unreachable states and forbidden solutions.
// Score field is 15 bits wide, min value is -2^14, we use half of that.
constexpr static int AwfulScore = -(1 << 13);

static bool is_awful(int S) {
    return S < AwfulScore / 2;
}

constexpr static int PerfectBonus = 4;  // Perfect per-pattern-char score.

FuzzyMatcher::FuzzyMatcher(llvm::StringRef pattern) :
    pat_n(std::min<int>(MaxPat, pattern.size())),
    score_scale(pat_n ? float{1} / (PerfectBonus * pat_n) : 0), word_n(0) {
    std::copy(pattern.begin(), pattern.begin() + pat_n, Pat);
    for(int I = 0; I < pat_n; ++I)
        low_pat[I] = lower(Pat[I]);
    scores[0][0][Miss] = {0, Miss};
    scores[0][0][Match] = {AwfulScore, Miss};

    for(int P = 0; P <= pat_n; ++P) {
        for(int W = 0; W < P; ++W) {
            for(Action A: {Miss, Match}) {
                scores[P][W][A] = {AwfulScore, Miss};
            }
        }
    }

    pat_type_set =
        calculate_roles(llvm::StringRef(Pat, pat_n), llvm::MutableArrayRef(pat_role, pat_n));
}

std::optional<float> FuzzyMatcher::match(llvm::StringRef word) {
    if(!(word_contains_pattern = init(word))) {
        return std::nullopt;
    }

    if(!pat_n) {
        return 1;
    }

    build_graph();
    auto best = std::max(scores[pat_n][word_n][Miss].score, scores[pat_n][word_n][Match].score);
    if(is_awful(best)) {
        return std::nullopt;
    }

    float score = score_scale * std::min(PerfectBonus * pat_n, std::max<int>(0, best));

    // If the pattern is as long as the word, we have an exact string match,
    // since every pattern character must match something.
    if(word_n == pat_n) {
        // May not be perfect 2 if case differs in a significant way.
        score *= 2;
    }

    return score;
}

// We get CharTypes from a lookup table. Each is 2 bits, 4 fit in each byte.
// The top 6 bits of the char select the byte, the bottom 2 select the offset.
// e.g. 'q' = 011100 01 = byte 28 (55), bits 3-2 (01) -> Lower.
constexpr static uint8_t CharTypes[] = {
    0x00, 0x00, 0x00, 0x00,                          // Control characters
    0x00, 0x00, 0x00, 0x00,                          // Control characters
    0xff, 0xff, 0xff, 0xff,                          // Punctuation
    0x55, 0x55, 0xf5, 0xff,                          // Numbers->Lower, more Punctuation.
    0xab, 0xaa, 0xaa, 0xaa,                          // @ and A-O
    0xaa, 0xaa, 0xea, 0xff,                          // P-Z, more Punctuation.
    0x57, 0x55, 0x55, 0x55,                          // ` and a-o
    0x55, 0x55, 0xd5, 0x3f,                          // p-z, Punctuation, DEL.
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // Bytes over 127 -> Lower.
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // (probably UTF-8).
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
};

// The Role can be determined from the Type of a character and its neighbors:
//
//   Example  | Chars | Type | Role
//   ---------+--------------+-----
//   F(o)oBar | Foo   | Ull  | Tail
//   Foo(B)ar | oBa   | lUl  | Head
//   (f)oo    | ^fo   | Ell  | Head
//   H(T)TP   | HTT   | UUU  | Tail
//
// Our lookup table maps a 6 bit key (Prev, Curr, Next) to a 2-bit Role.
// A byte packs 4 Roles. (Prev, Curr) selects a byte, Next selects the offset.
// e.g. Lower, Upper, Lower -> 01 10 01 -> byte 6 (aa), bits 3-2 (10) -> Head.
constexpr static uint8_t CharRoles[] = {
    // clang-format off
    //         Curr= Empty Lower Upper Separ
    /* Prev=Empty */ 0x00, 0xaa, 0xaa, 0xff, // At start, Lower|Upper->Head
    /* Prev=Lower */ 0x00, 0x55, 0xaa, 0xff, // In word, Upper->Head;Lower->Tail
    /* Prev=Upper */ 0x00, 0x55, 0x59, 0xff, // Ditto, but U(U)U->Tail
    /* Prev=Separ */ 0x00, 0xaa, 0xaa, 0xff, // After separator, like at start
    // clang-format on
};

template <typename T>
static T packed_lookup(const uint8_t* Data, int I) {
    return static_cast<T>((Data[I >> 2] >> ((I & 3) * 2)) & 3);
}

CharTypeSet calculate_roles(llvm::StringRef text, llvm::MutableArrayRef<CharRole> roles) {
    assert(text.size() == roles.size());
    if(text.size() == 0) {
        return 0;
    }

    CharType type = packed_lookup<CharType>(CharTypes, text[0]);
    CharTypeSet TypeSet = 1 << type;
    // Types holds a sliding window of (Prev, Curr, Next) types.
    // Initial value is (Empty, Empty, type of Text[0]).
    int types = type;
    // Rotate slides in the type of the next character.
    auto rotate = [&](CharType T) {
        types = ((types << 2) | T) & 0x3f;
    };

    for(unsigned I = 0; I < text.size() - 1; ++I) {
        // For each character, rotate in the next, and look up the role.
        type = packed_lookup<CharType>(CharTypes, text[I + 1]);
        TypeSet |= 1 << type;
        rotate(type);
        roles[I] = packed_lookup<CharRole>(CharRoles, types);
    }

    // For the last character, the "next character" is Empty.
    rotate(Empty);
    roles[text.size() - 1] = packed_lookup<CharRole>(CharRoles, types);
    return TypeSet;
}

// Sets up the data structures matching Word.
// Returns false if we can cheaply determine that no match is possible.
bool FuzzyMatcher::init(llvm::StringRef new_word) {
    word_n = std::min<int>(MaxWord, new_word.size());
    if(pat_n > word_n) {
        return false;
    }

    std::copy(new_word.begin(), new_word.begin() + word_n, word);
    if(pat_n == 0) {
        return true;
    }

    for(int I = 0; I < word_n; ++I) {
        low_word[I] = lower(word[I]);
    }

    // Cheap subsequence check.
    for(int W = 0, P = 0; P != pat_n; ++W) {
        if(W == word_n) {
            return false;
        }

        if(low_word[W] == low_pat[P]) {
            ++P;
        }
    }

    // FIXME: some words are hard to tokenize algorithmically.
    // e.g. vsprintf is V S Print F, and should match [pri] but not [int].
    // We could add a tokenization dictionary for common stdlib names.
    word_type_set =
        calculate_roles(llvm::StringRef(word, word_n), llvm::MutableArrayRef(word_role, word_n));
    return true;
}

// The forwards pass finds the mappings of Pattern onto Word.
// Score = best score achieved matching Word[..W] against Pat[..P].
// Unlike other tables, indices range from 0 to N *inclusive*
// Matched = whether we chose to match Word[W] with Pat[P] or not.
//
// Points are mostly assigned to matched characters, with 1 being a good score
// and 3 being a great one. So we treat the score range as [0, 3 * PatN].
// This range is not strict: we can apply larger bonuses/penalties, or penalize
// non-matched characters.
void FuzzyMatcher::build_graph() {
    for(int W = 0; W < word_n; ++W) {
        scores[0][W + 1][Miss] = {scores[0][W][Miss].score - skip_penalty(W, Miss), Miss};
        scores[0][W + 1][Match] = {AwfulScore, Miss};
    }

    for(int P = 0; P < pat_n; ++P) {
        for(int W = P; W < word_n; ++W) {
            auto &score = scores[P + 1][W + 1], &PreMiss = scores[P + 1][W];

            auto match_miss_score = PreMiss[Match].score;
            auto miss_miss_score = PreMiss[Miss].score;
            if(P < pat_n - 1) {  // Skipping trailing characters is always free.
                match_miss_score -= skip_penalty(W, Match);
                miss_miss_score -= skip_penalty(W, Miss);
            }
            score[Miss] = (match_miss_score > miss_miss_score) ? ScoreInfo{match_miss_score, Match}
                                                               : ScoreInfo{miss_miss_score, Miss};

            auto& pre_match = scores[P][W];
            auto match_match_score = allow_match(P, W, Match)
                                         ? pre_match[Match].score + match_bonus(P, W, Match)
                                         : AwfulScore;
            auto miss_match_score = allow_match(P, W, Miss)
                                        ? pre_match[Miss].score + match_bonus(P, W, Miss)
                                        : AwfulScore;
            score[Match] = (match_match_score > miss_match_score)
                               ? ScoreInfo{match_match_score, Match}
                               : ScoreInfo{miss_match_score, Miss};
        }
    }
}

bool FuzzyMatcher::allow_match(int P, int W, Action last) const {
    if(low_pat[P] != low_word[W]) {
        return false;
    }

    // We require a "strong" match:
    // - for the first pattern character.  [foo] !~ "barefoot"
    // - after a gap.                      [pat] !~ "patnther"
    if(last == Miss) {
        // We're banning matches outright, so conservatively accept some other cases
        // where our segmentation might be wrong:
        //  - allow matching B in ABCDef (but not in NDEBUG)
        //  - we'd like to accept print in sprintf, but too many false positives
        if(word_role[W] == Tail && (word[W] == low_word[W] || !(word_type_set & 1 << Lower))) {
            return false;
        }
    }
    return true;
}

int FuzzyMatcher::skip_penalty(int W, Action Last) const {
    // Skipping the first character.
    if(W == 0) {
        return 3;
    }

    // Skipping a segment. We want to keep this lower than a consecutive match bonus.
    // Instead of penalizing non-consecutive matches, we give a bonus to a
    // consecutive match in matchBonus. This produces a better score distribution
    // than penalties in case of small patterns, e.g. 'up' for 'unique_ptr'.
    if(word_role[W] == Head) {
        return 1;
    }

    return 0;
}

int FuzzyMatcher::match_bonus(int P, int W, Action last) const {
    assert(low_pat[P] == low_word[W]);
    int S = 1;
    bool is_pat_single_case = (pat_type_set == 1 << Lower) || (pat_type_set == 1 << Upper);
    // Bonus: case matches, or a Head in the pattern aligns with one in the word.
    // Single-case patterns lack segmentation signals and we assume any character
    // can be a head of a segment.
    if(Pat[P] == word[W] || (word_role[W] == Head && (is_pat_single_case || pat_role[P] == Head))) {
        ++S;
    }

    // Bonus: a consecutive match. First character match also gets a bonus to
    // ensure prefix final match score normalizes to 1.0.
    if(W == 0 || last == Match) {
        S += 2;
    }

    // Penalty: matching inside a segment (and previous char wasn't matched).
    if(word_role[W] == Tail && P && last == Miss) {
        S -= 3;
    }

    // Penalty: a Head in the pattern matches in the middle of a word segment.
    if(pat_role[P] == Head && word_role[W] == Tail) {
        --S;
    }

    // Penalty: matching the first pattern character in the middle of a segment.
    if(P == 0 && word_role[W] == Tail) {
        S -= 4;
    }

    assert(S <= PerfectBonus);
    return S;
}

llvm::SmallString<256> FuzzyMatcher::dumpLast(llvm::raw_ostream& OS) const {
    llvm::SmallString<256> result;
    OS << "=== Match \"" << llvm::StringRef(word, word_n) << "\" against ["
       << llvm::StringRef(Pat, pat_n) << "] ===\n";
    if(pat_n == 0) {
        OS << "Pattern is empty: perfect match.\n";
        return result = llvm::StringRef(word, word_n);
    }

    if(word_n == 0) {
        OS << "Word is empty: no match.\n";
        return result;
    }

    if(!word_contains_pattern) {
        OS << "Substring check failed.\n";
        return result;
    }

    if(is_awful(std::max(scores[pat_n][word_n][Match].score, scores[pat_n][word_n][Miss].score))) {
        OS << "Substring check passed, but all matches are forbidden\n";
    }

    if(!(pat_type_set & 1 << Upper)) {
        OS << "Lowercase query, so scoring ignores case\n";
    }

    // Traverse Matched table backwards to reconstruct the Pattern/Word mapping.
    // The Score table has cumulative scores, subtracting along this path gives
    // us the per-letter scores.
    Action last =
        (scores[pat_n][word_n][Match].score > scores[pat_n][word_n][Miss].score) ? Match : Miss;
    int S[MaxWord];
    Action A[MaxWord];
    for(int W = word_n - 1, P = pat_n - 1; W >= 0; --W) {
        A[W] = last;
        const auto& Cell = scores[P + 1][W + 1][last];
        if(last == Match)
            --P;
        const auto& Prev = scores[P + 1][W][Cell.Prev];
        S[W] = Cell.score - Prev.score;
        last = Cell.Prev;
    }
    for(int I = 0; I < word_n; ++I) {
        if(A[I] == Match && (I == 0 || A[I - 1] == Miss)) {
            result.push_back('[');
        }

        if(A[I] == Miss && I > 0 && A[I - 1] == Match) {
            result.push_back(']');
        }

        result.push_back(word[I]);
    }

    if(A[word_n - 1] == Match) {
        result.push_back(']');
    }

    for(char C: llvm::StringRef(word, word_n))
        OS << " " << C << " ";
    OS << "\n";
    for(int I = 0, J = 0; I < word_n; I++)
        OS << " " << (A[I] == Match ? Pat[J++] : ' ') << " ";
    OS << "\n";
    for(int I = 0; I < word_n; I++)
        OS << llvm::format("%2d ", S[I]);
    OS << "\n";

    OS << "\nSegmentation:";
    OS << "\n'" << llvm::StringRef(word, word_n) << "'\n ";
    for(int I = 0; I < word_n; ++I)
        OS << "?-+ "[static_cast<int>(word_role[I])];
    OS << "\n[" << llvm::StringRef(Pat, pat_n) << "]\n ";
    for(int I = 0; I < pat_n; ++I)
        OS << "?-+ "[static_cast<int>(pat_role[I])];
    OS << "\n";

    OS << "\nScoring table (last-Miss, last-Match):\n";
    OS << " |    ";
    for(char C: llvm::StringRef(word, word_n))
        OS << "  " << C << " ";
    OS << "\n";
    OS << "-+----" << std::string(word_n * 4, '-') << "\n";
    for(int I = 0; I <= pat_n; ++I) {
        for(Action A: {Miss, Match}) {
            OS << ((I && A == Miss) ? Pat[I - 1] : ' ') << "|";
            for(int J = 0; J <= word_n; ++J) {
                if(!is_awful(scores[I][J][A].score))
                    OS << llvm::format("%3d%c",
                                       scores[I][J][A].score,
                                       scores[I][J][A].Prev == Match ? '*' : ' ');
                else
                    OS << "    ";
            }
            OS << "\n";
        }
    }

    return result;
}

}  // namespace clice
