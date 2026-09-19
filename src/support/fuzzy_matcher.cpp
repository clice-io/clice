#include "support/fuzzy_matcher.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace clice {

namespace {

/// Characters past these are ignored, bounding the cost of a match.
constexpr std::size_t max_pattern = 63;
constexpr std::size_t max_name = name_bound;

enum class CharClass : std::uint8_t {
    Separator,
    Lower,
    Upper,
};

CharClass classify(char c) {
    auto byte = static_cast<unsigned char>(c);
    if(byte >= 'A' && byte <= 'Z') {
        return CharClass::Upper;
    }
    if((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte >= 128) {
        return CharClass::Lower;
    }
    return CharClass::Separator;
}

char lower(char c) {
    return classify(c) == CharClass::Upper ? static_cast<char>(c + ('a' - 'A')) : c;
}

bool is_upper(char c) {
    return classify(c) == CharClass::Upper;
}

bool has_lowercase_letter(llvm::StringRef text) {
    return llvm::any_of(text, [](char c) { return c >= 'a' && c <= 'z'; });
}

/// Whether a match may land on the character after a gap: a word head, a
/// separator, or an uppercase letter of an initialism inside a
/// mixed-case name. `has_lower` is whether the name has any lowercase
/// letter: without one there is no initialism to speak of, and `b` inside
/// `NDEBUG` is a plain start inside a word.
bool is_anchor(char c, CharRole role, bool has_lower) {
    return role != CharRole::Tail || (is_upper(c) && has_lower);
}

/// Match points. A pattern character earns up to `perfect` when it lands
/// on a head or continues a run and its case agrees; the penalties keep
/// a prefix at exactly `perfect` per character and everything else
/// below, with a run resumed after a gap costing more than a skipped
/// word so a contiguous match wins over a scattered one.
constexpr int perfect = 4;
constexpr int anchored_or_contiguous = 3;
constexpr int case_agrees = 1;
constexpr int gap = 2;
constexpr int start_inside_word = 2;
constexpr int skipped_head = 1;
constexpr int skipped_first = 2;
constexpr int typo = 3;

/// Marks a state no path reaches; far below any reachable score, which
/// is bounded by the name length.
constexpr std::int16_t unreachable = -10000;

NameToken unigram(char a) {
    return static_cast<unsigned char>(a);
}

NameToken bigram(char a, char b) {
    return (unigram(a) << 8) | unigram(b);
}

NameToken trigram(char a, char b, char c) {
    return (bigram(a, b) << 8) | unigram(c);
}

void sort_unique(llvm::SmallVectorImpl<NameToken>& tokens) {
    llvm::sort(tokens);
    tokens.erase(llvm::unique(tokens), tokens.end());
}

/// The lowercase letters of a pattern in order, separators dropped —
/// the sequence a query's tokens are formed from.
llvm::SmallVector<char, 32> pattern_letters(llvm::StringRef pattern) {
    llvm::SmallVector<char, 32> letters;
    for(char c: pattern) {
        if(classify(c) != CharClass::Separator) {
            letters.push_back(lower(c));
        }
    }
    return letters;
}

}  // namespace

void segment(llvm::StringRef text, llvm::MutableArrayRef<CharRole> roles) {
    assert(text.size() == roles.size());
    auto prev = CharClass::Separator;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        auto cur = classify(text[i]);
        auto next = i + 1 < text.size() ? classify(text[i + 1]) : CharClass::Separator;
        if(cur == CharClass::Separator) {
            roles[i] = CharRole::Separator;
        } else if(prev == CharClass::Separator) {
            roles[i] = CharRole::Head;
        } else if(cur == CharClass::Upper &&
                  (prev == CharClass::Lower || next == CharClass::Lower)) {
            roles[i] = CharRole::Head;
        } else {
            roles[i] = CharRole::Tail;
        }
        prev = cur;
    }
}

FuzzyMatcher::FuzzyMatcher(llvm::StringRef pattern, MatchOptions options) :
    options(options), whole_pattern(pattern.size() <= max_pattern) {
    pattern = pattern.take_front(max_pattern);
    pat.assign(pattern.begin(), pattern.end());
    for(char c: pat) {
        low_pat.push_back(lower(c));
        pat_has_upper = pat_has_upper || is_upper(c);
    }
    cells.resize((pat.size() + 1) * (max_name + 1) * 6);
}

FuzzyMatcher::Cell& FuzzyMatcher::cell(std::size_t p, std::size_t n, Run run, bool typo) {
    return cells[((p * (max_name + 1) + n) * 3 + static_cast<std::size_t>(run)) * 2 + typo];
}

bool FuzzyMatcher::prepare(llvm::StringRef text) {
    whole_name = text.size() <= max_name;
    text = text.take_front(max_name);
    if(pat.size() > text.size() + (options.typo ? 1 : 0)) {
        return false;
    }
    name.assign(text.begin(), text.end());
    low_name.clear();
    for(char c: name) {
        low_name.push_back(lower(c));
    }
    // The letters must appear in order for a match to exist; with a
    // typo allowed the full table has to judge.
    if(!options.typo) {
        std::size_t p = 0;
        for(char c: low_name) {
            if(p < low_pat.size() && low_pat[p] == c) {
                p += 1;
            }
        }
        if(p < low_pat.size()) {
            return false;
        }
    }
    role.resize(name.size());
    segment(text, role);
    bool has_lower = has_lowercase_letter(text);
    anchor.clear();
    for(std::size_t i = 0; i < name.size(); i += 1) {
        anchor.push_back(is_anchor(name[i], role[i], has_lower));
    }
    return true;
}

void FuzzyMatcher::fill() {
    constexpr Run runs[] = {Run::Gap, Run::Free, Run::Inside};
    auto plen = pat.size();
    auto nlen = name.size();
    for(std::size_t p = 0; p <= plen; p += 1) {
        for(std::size_t n = 0; n <= nlen; n += 1) {
            for(auto run: runs) {
                for(bool spent: {false, true}) {
                    cell(p, n, run, spent) = {unreachable, Step::None, Run::Gap, false};
                }
            }
        }
    }
    cell(0, 0, Run::Gap, false).score = 0;

    auto relax = [](Cell& target, int score, Step step, Run prev_run, bool prev_typo) {
        if(score > target.score) {
            target = {static_cast<std::int16_t>(score), step, prev_run, prev_typo};
        }
    };
    // A lowercase pattern carries no case of its own: landing on an
    // uppercase head is as good as agreeing.
    auto case_agreement = [&](std::size_t p, std::size_t n) {
        return pat[p] == name[n] || (!pat_has_upper && is_upper(name[n]) && anchor[n]);
    };
    // The run state after consuming name character `n` in a match.
    auto continued = [&](std::size_t p, std::size_t n, Run run) {
        if(p == 0 && !anchor[n]) {
            return Run::Inside;
        }
        if(run == Run::Inside && role[n] == CharRole::Tail) {
            return Run::Inside;
        }
        return Run::Free;
    };

    // Every step leads to a state later in this order, so each state is
    // final when its own steps are taken.
    for(std::size_t n = 0; n <= nlen; n += 1) {
        for(std::size_t p = 0; p <= plen; p += 1) {
            for(auto run: runs) {
                for(bool spent: {false, true}) {
                    int score = cell(p, n, run, spent).score;
                    if(score == unreachable) {
                        continue;
                    }
                    // Skipping a name character: skipped heads cost, and
                    // skipping the very first character costs extra so a
                    // prefix beats a match further in. Once the pattern is
                    // consumed the rest is free; a run begun inside a word
                    // may not skip the rest of that word.
                    if(n < nlen && !(run == Run::Inside && p < plen && role[n] == CharRole::Tail)) {
                        int cost = 0;
                        if(p < plen) {
                            cost += role[n] == CharRole::Head ? skipped_head : 0;
                            cost += n == 0 ? skipped_first : 0;
                        }
                        relax(cell(p, n + 1, Run::Gap, spent),
                              score - cost,
                              Step::Skip,
                              run,
                              spent);
                    }
                    if(p < plen && n < nlen && low_pat[p] == low_name[n]) {
                        bool contiguous = run != Run::Gap;
                        int points = 0;
                        if(anchor[n] || contiguous) {
                            points += anchored_or_contiguous;
                        }
                        if(case_agreement(p, n)) {
                            points += case_agrees;
                        }
                        if(p == 0 && !anchor[n]) {
                            points -= start_inside_word;
                        }
                        auto& target = cell(p + 1, n + 1, continued(p, n, run), spent);
                        if((p == 0 && options.inside_word) || contiguous || anchor[n]) {
                            if(!contiguous && p > 0) {
                                points -= gap;
                            }
                            relax(target, score + points, Step::Match, run, spent);
                        } else if(options.typo && !spent) {
                            // Landing inside a word after a gap: the name
                            // has characters the pattern does not.
                            relax(cell(p + 1, n + 1, Run::Free, true),
                                  score + points - typo,
                                  Step::Match,
                                  run,
                                  false);
                        }
                    }
                    if(options.typo && !spent && p < plen) {
                        relax(cell(p + 1, n, run, true), score - typo, Step::Drop, run, false);
                        if(n < nlen && low_pat[p] != low_name[n]) {
                            relax(cell(p + 1, n + 1, continued(p, n, run), true),
                                  score - typo,
                                  Step::Replace,
                                  run,
                                  false);
                        }
                    }
                }
            }
        }
    }
}

/// The best complete match, a clean one whenever there is one: a match
/// spending the typo allowance is judged only when no clean path exists.
std::optional<std::pair<int, FuzzyMatcher::Cell*>> FuzzyMatcher::best() {
    for(bool spent: {false, true}) {
        std::optional<std::pair<int, Cell*>> result;
        for(auto run: {Run::Gap, Run::Free, Run::Inside}) {
            auto& end = cell(pat.size(), name.size(), run, spent);
            if(end.score != unreachable && (!result || end.score > result->first)) {
                result = {end.score, &end};
            }
        }
        if(result) {
            return result;
        }
    }
    return std::nullopt;
}

std::optional<float> FuzzyMatcher::match(llvm::StringRef text) {
    if(!prepare(text)) {
        return std::nullopt;
    }
    if(pat.empty()) {
        return 1;
    }
    fill();
    auto found = best();
    if(!found) {
        return std::nullopt;
    }
    auto [raw, end] = *found;
    // Penalties can sink a match below zero; it stays a match, at the
    // smallest positive score so quality still orders such results.
    int ceiling = perfect * static_cast<int>(pat.size());
    float score = static_cast<float>(std::clamp(raw, 1, ceiling)) / static_cast<float>(ceiling);
    // The end cell's typo flag is the last dimension of its index.
    bool spent = (end - cells.data()) % 2 == 1;
    if(spent) {
        return score / 2;
    }
    // Only the whole pattern spelling the whole name is exact: a match
    // within the truncation bounds is not.
    if(whole_pattern && whole_name && pat.size() == name.size()) {
        return score * 2;
    }
    return score;
}

llvm::SmallVector<std::uint8_t, 32> FuzzyMatcher::matched_positions() {
    llvm::SmallVector<std::uint8_t, 32> positions;
    auto found = best();
    if(!found) {
        return positions;
    }
    std::size_t p = pat.size();
    std::size_t n = name.size();
    Cell* current = found->second;
    while(current->step != Step::None) {
        auto prev_run = current->prev_run;
        bool prev_typo = current->prev_typo;
        switch(current->step) {
            case Step::Skip: n -= 1; break;
            case Step::Match:
                p -= 1;
                n -= 1;
                positions.push_back(static_cast<std::uint8_t>(n));
                break;
            case Step::Drop: p -= 1; break;
            case Step::Replace:
                p -= 1;
                n -= 1;
                break;
            case Step::None: std::unreachable();
        }
        current = &cell(p, n, prev_run, prev_typo);
    }
    std::ranges::reverse(positions);
    return positions;
}

std::string FuzzyMatcher::annotate(llvm::StringRef text) {
    if(!match(text)) {
        return {};
    }
    auto positions = matched_positions();
    std::string result;
    std::size_t next = 0;
    bool open = false;
    for(std::size_t i = 0; i < name.size(); i += 1) {
        bool hit = next < positions.size() && positions[next] == i;
        if(hit != open) {
            result += hit ? '[' : ']';
            open = hit;
        }
        if(hit) {
            next += 1;
        }
        result += name[i];
    }
    if(open) {
        result += ']';
    }
    return result;
}

void name_tokens(llvm::StringRef name, llvm::SmallVectorImpl<NameToken>& out) {
    out.clear();
    name = name.take_front(max_name);
    if(name.empty()) {
        return;
    }
    llvm::SmallVector<CharRole, 64> roles(name.size());
    segment(name, roles);
    bool has_lower = has_lowercase_letter(name);

    // The positions a match may jump to from anywhere before them.
    llvm::SmallVector<std::uint32_t, 32> anchors;
    for(std::size_t i = 0; i < name.size(); i += 1) {
        if(roles[i] != CharRole::Separator && is_anchor(name[i], roles[i], has_lower)) {
            anchors.push_back(static_cast<std::uint32_t>(i));
        }
    }
    // Where the matcher may go from position `i`: on to the next
    // character of the same word, or to any later anchor.
    auto successors = [&](std::size_t i) {
        llvm::SmallVector<std::uint32_t, 16> next;
        if(i + 1 < name.size() && roles[i + 1] == CharRole::Tail) {
            next.push_back(static_cast<std::uint32_t>(i + 1));
        }
        auto later = std::ranges::upper_bound(anchors, static_cast<std::uint32_t>(i));
        for(auto it = later; it != anchors.end(); it += 1) {
            if(next.empty() || next.front() != *it) {
                next.push_back(*it);
            }
        }
        return next;
    };

    std::size_t heads_seen = 0;
    for(std::size_t i = 0; i < name.size(); i += 1) {
        if(roles[i] == CharRole::Separator) {
            continue;
        }
        auto first = successors(i);
        bool short_head = roles[i] == CharRole::Head && heads_seen < 2;
        if(short_head) {
            heads_seen += 1;
            out.push_back(unigram(lower(name[i])));
        }
        for(auto j: first) {
            if(short_head) {
                out.push_back(bigram(lower(name[i]), lower(name[j])));
            }
            for(auto k: successors(j)) {
                out.push_back(trigram(lower(name[i]), lower(name[j]), lower(name[k])));
            }
        }
    }
    sort_unique(out);
}

void query_tokens(llvm::StringRef pattern, llvm::SmallVectorImpl<NameToken>& out) {
    out.clear();
    auto letters = pattern_letters(pattern.take_front(max_pattern));
    if(letters.size() >= 3) {
        for(std::size_t i = 0; i + 2 < letters.size(); i += 1) {
            out.push_back(trigram(letters[i], letters[i + 1], letters[i + 2]));
        }
    } else if(letters.size() == 2) {
        out.push_back(bigram(letters[0], letters[1]));
    } else if(letters.size() == 1) {
        out.push_back(unigram(letters[0]));
    }
    sort_unique(out);
}

void typo_tokens(llvm::StringRef pattern, llvm::SmallVectorImpl<TypoAlternative>& out) {
    out.clear();
    auto letters = pattern_letters(pattern.take_front(max_pattern));
    if(letters.size() < 6) {
        return;
    }
    for(std::size_t wrong = 0; wrong < letters.size(); wrong += 1) {
        TypoAlternative alternative;
        for(std::size_t i = 0; i + 2 < letters.size(); i += 1) {
            if(i + 2 < wrong || i > wrong) {
                alternative.tokens.push_back(trigram(letters[i], letters[i + 1], letters[i + 2]));
            }
        }
        sort_unique(alternative.tokens);
        out.push_back(std::move(alternative));
    }
}

}  // namespace clice
