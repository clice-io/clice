#include <format>

#include "Support/GlobPattern.h"

namespace clice {

// Expands character ranges and returns a bitmap.
// For example, "a-cf-hz" is expanded to "abcfghz".
static std::expected<GlobCharSet, std::string> expand(llvm::StringRef s, llvm::StringRef original) {
    GlobCharSet bv{false};

    for(size_t i = 0, e = s.size(); i < e; ++i) {
        switch(s[i]) {
            case '\\': {
                ++i;
                if(i == e) {
                    return std::unexpected{"Invalid expansions: stary `\\`"};
                }
                if(s[i] != '/') {
                    bv.set(s[i], true);
                }
                break;
            }

            case '-': {
                if(i == 0 || i + 1 == e) {
                    bv.set('-', true);
                    break;
                }
                char c_begin = s[i - 1];
                char c_end = s[i + 1];
                ++i;
                if(c_end == '\\') {
                    ++i;
                    if(s[i] == e) {
                        return std::unexpected{"Invalid expansions: stary `\\`"};
                    }
                    c_end = s[i];
                }
                if(c_begin > c_end) {
                    return std::unexpected{
                        std::format("Invalid expansion: `{}` is larger than `{}`", c_begin, c_end)};
                }
                for(char c = c_begin; c <= c_end; ++c) {
                    if(c != '/') {
                        bv.set(c, true);
                    }
                }
                break;
            }

            default: {
                if(s[i] != '/') {
                    bv.set(s[i], true);
                }
            }
        }
    }

    return bv;
}

// Identify brace expansions in S and return the list of patterns they expand
// into.
static std::expected<llvm::SmallVector<std::string, 1>, std::string>
    parseBraceExpansions(llvm::StringRef s, size_t max_subpattern_num) {
    llvm::SmallVector<std::string> subpatterns = {s.str()};
    if(max_subpattern_num == 0 || !s.contains('{')) {
        return subpatterns;
    }

    struct BraceExpansion {
        size_t start;
        size_t length;
        llvm::SmallVector<llvm::StringRef, 2> terms;
    };

    llvm::SmallVector<BraceExpansion, 0> brace_expansions;

    BraceExpansion* current_be = nullptr;
    size_t term_begin = 0;
    for(size_t i = 0, e = s.size(); i != e; ++i) {
        if(s[i] == '[') {
            ++i;
            if(s[i] == ']') {
                ++i;
            }
            while(i != e && s[i] != ']') {
                if(s[i] == '\\') {
                    if(i == e) {
                        return std::unexpected{
                            "Invalid glob pattern, unmatched '[', with stray " "'\\' inside"};
                    }
                    ++i;
                }
                ++i;
            }
            if(i == e) {
                return std::unexpected{"Invalid glob pattern, unmatched '['"};
            }
        } else if(s[i] == '{') {
            if(current_be) {
                return std::unexpected{"Nested brace expansions are not supported"};
            }
            current_be = &brace_expansions.emplace_back();
            current_be->start = i;
            term_begin = i + 1;
        } else if(s[i] == ',') {
            if(!current_be) {
                continue;
            }
            current_be->terms.push_back(s.substr(term_begin, i - term_begin));
            term_begin = i + 1;
        } else if(s[i] == '}') {
            if(!current_be) {
                continue;
            }
            if(current_be->terms.empty() && i - term_begin == 0) {
                return std::unexpected{"Empty brace expression is not supported"};
            }
            current_be->terms.push_back(s.substr(term_begin, i - term_begin));
            current_be->length = i - current_be->start + 1;
            current_be = nullptr;
        } else if(s[i] == '\\') {
            ++i;
            if(i == e) {
                return std::unexpected{"Invalid glob pattern, stray '\\'"};
            }
        }
    }

    if(current_be) {
        return std::unexpected{"Incomplete brace expansion"};
    }

    size_t subpattern_num = 1;
    for(auto& be: brace_expansions) {
        if(subpattern_num > std::numeric_limits<size_t>::max() / be.terms.size()) {
            subpattern_num = std::numeric_limits<size_t>::max();
            break;
        }
        subpattern_num *= be.terms.size();
    }

    if(subpattern_num > max_subpattern_num) {
        return std::unexpected{"Too many brace expansions"};
    }

    for(auto& be: reverse(brace_expansions)) {
        llvm::SmallVector<std::string> orig_sub_patterns;
        std::swap(subpatterns, orig_sub_patterns);
        for(llvm::StringRef term: be.terms) {
            for(llvm::StringRef orig: orig_sub_patterns) {
                subpatterns.emplace_back(orig).replace(be.start, be.length, term);
            }
        }
    }

    return subpatterns;
}

std::expected<GlobPattern, std::string> GlobPattern::create(llvm::StringRef s,
                                                            size_t max_subpattern_num) {
    // Store the prefix that does not contain any metacharacter.
    GlobPattern pat;
    size_t prefix_size = s.find_first_of("?*[{\\");
    if(prefix_size == std::string::npos) {
        pat.prefix = s.substr(0, prefix_size).str();
        // check if there is multiple `/` in prefix
        size_t last_slash = 0;
        size_t size = pat.prefix.size();
        for(size_t i = 0; i < size; ++i) {
            if(s[i] == '/' && i - last_slash == 1) {
                return std::unexpected{"Multiple `/` is not allowed"};
            }
            last_slash = i;
        }
        return pat;
    }
    if(prefix_size != 0 && s[prefix_size - 1] == '/') {
        pat.prefix_at_seg_end = true;
        --prefix_size;
    }
    pat.prefix = s.substr(0, prefix_size).str();
    // check if there is multiple `/` in prefix
    size_t last_slash = 0;
    size_t size = pat.prefix.size();
    for(size_t i = 0; i < size; ++i) {
        if(s[i] == '/' && i - last_slash == 1) {
            return std::unexpected{"Multiple `/` is not allowed"};
        }
        last_slash = i;
    }
    s = s.substr(pat.prefix_at_seg_end ? prefix_size + 1 : prefix_size);

    llvm::SmallVector<std::string, 1> sub_pats;
    if(auto res = parseBraceExpansions(s, max_subpattern_num); res.has_value()) {
        sub_pats = res.value();
    } else {
        return std::unexpected{res.error()};
    }

    for(auto sub_pat: sub_pats) {
        if(auto res = SubGlobPattern::create(sub_pat); res.has_value()) {
            pat.sub_globs.push_back(res.value());
        } else {
            return std::unexpected{res.error()};
        }
    }

    return pat;
}

std::expected<GlobPattern::SubGlobPattern, std::string>
    GlobPattern::SubGlobPattern::create(llvm::StringRef s) {
    SubGlobPattern pat;
    llvm::SmallVector<GlobSegment, 0> glob_segments;
    GlobSegment* current_gs = &glob_segments.emplace_back();
    current_gs->start = 0;
    pat.pat.assign(s.begin(), s.end());
    // Parse brackets.
    size_t e = s.size();
    for(size_t i = 0; i < e; ++i) {
        if(!current_gs) {
            current_gs = &glob_segments.emplace_back();
            current_gs->start = i;
        }
        if(s[i] == '[') {
            ++i;
            size_t j = i;
            if(s[j] == ']') {
                // ']' is allowed as the first character of a character class. '[]'
                // is invalid. So, just skip the first character.
                ++j;
            }
            while(j != e && s[j] != ']') {
                // Check if there is invalid escape char
                ++j;
                if(s[j - 1] == '\\') {
                    if(j == e) {
                        // Reach here in such case:
                        // [a-zA-Z\]
                        // ~~~~~~~~^
                        return std::unexpected{
                            "Invalid glob pattern, unmatched '[' with stray '\\' inside"};
                    }
                    ++j;
                }
            }
            if(j == e) {
                return std::unexpected{"Invalid glob pattern, unmatched '['"};
            }
            llvm::StringRef chars = s.substr(i, j - i);
            bool invert = s[i] == '^' || s[i] == '!';
            auto bv = invert ? expand(chars.substr(1), s) : expand(chars, s);
            if(!bv.has_value()) {
                return std::unexpected{bv.error()};
            }
            if(invert) {
                bv->flip();
            }
            pat.brackets.push_back(Bracket{j + 1, bv.value()});
            i = j;
        } else if(s[i] == '\\') {
            if(++i == e) {
                return std::unexpected{"Invalid glob pattern, stray '\\'"};
            }
        } else if(s[i] == '/') {
            if(i - current_gs->start == 1) {
                return std::unexpected{"Multiple `/` is not allowed"};
            }
            current_gs->end = i;
            current_gs = nullptr;
        } else if(s[i] == '*') {
            // check if there is multiple `*`
            if(i + 1 < e - 1 && s[i + 1] == '*' && s[i + 2] == '*') {
                return std::unexpected{"Multiple `*` is not allowed"};
            }
        }
    }

    if(current_gs) {
        current_gs->end = e;
        current_gs = nullptr;
    }

    std::swap(pat.glob_segments, glob_segments);
    return pat;
}

bool GlobPattern::match(llvm::StringRef str) {
    if(!str.consume_front(prefix)) {
        return false;
    }

    if(str.empty() && sub_globs.empty()) {
        return true;
    }

    if(!str.empty() && prefix_at_seg_end) {
        if(str[0] != '/') {
            return false;
        }
        str = str.substr(1);
    }

    for(auto& Glob: sub_globs) {
        if(Glob.match(str)) {
            return true;
        }
    }
    return false;
}

bool GlobPattern::SubGlobPattern::match(llvm::StringRef str) const {
    // p: Current position in the pattern
    // s: Current position in the String
    // b: Current processed bracket num
    // s_start: String start
    // s_end: String end
    // p_start: Pattern start
    // p_end: Pattern end
    // seg_start: Position of the start of current segment
    // s_end: Position of the end of current segment
    // b: Current matched bracket
    // current_glob_seg: Index of glob patterns
    // wild_mode: Whether wildcards are allowed to cross segments
    const char* s = str.data();
    const char* const s_start = s;
    const char* const s_end = s + str.size();
    const char* p = pat.data();
    const char* seg_start = p;
    const char* const p_start = p;
    const char* const p_end = p + pat.size();
    const char* seg_end = p + glob_segments[0].end;
    size_t b = 0;
    size_t current_glob_seg = 0;
    bool wild_mode = false;

    // Status for backtracing
    struct BacktraceStat {
        size_t b;
        size_t glob_seg;
        bool wild_mode;
        const char* p;
        const char* s;
        const char* seg_end;
        const char* seg_start;
    };

    llvm::SmallVector<BacktraceStat, 6> backtrace_stack;
    const size_t seg_num = glob_segments.size();

    auto save_stat =
        [&backtrace_stack, &b, &current_glob_seg, &wild_mode, &p, &s, &seg_end, &seg_start]() {
            backtrace_stack.push_back({.b = b,
                                       .glob_seg = current_glob_seg,
                                       .wild_mode = wild_mode,
                                       .p = p,
                                       .s = s,
                                       .seg_end = seg_end,
                                       .seg_start = seg_start});
        };

    while(current_glob_seg < seg_num) {

        if(s == s_end) {
            // Return true if all pattern characters are processed or only
            // '*' or '/' characters remain
            return getPat().find_first_not_of("*/", p - pat.data()) == std::string::npos;
        }

        if(p != seg_end) {
            switch(*p) {
                case '*': {
                    if(p + 1 != p_end && *(p + 1) == '*') {
                        // Met '**'
                        p += 2;
                        wild_mode = true;
                        // Consume extra '*'
                        while(p != p_end && (*p == '*' || *p == '/')) {
                            if(*p == '/') {
                                if(current_glob_seg + 1 == seg_num) {
                                    return true;
                                }
                                ++current_glob_seg;
                                seg_start = p_start + glob_segments[current_glob_seg].start;
                                seg_end = p_start + glob_segments[current_glob_seg].end;
                            }
                            ++p;
                        }
                        if(p == seg_end) {
                            // '**' at segment end
                            if(current_glob_seg + 1 == seg_num) {
                                // '**' at pattern end
                                return true;
                            }

                            // Goto next segment
                            ++current_glob_seg;
                            while(s != s_end && *s == '/') {
                                ++s;
                            }
                            p = p_start + glob_segments[current_glob_seg].start;
                            seg_start = p;
                            seg_end = p_start + glob_segments[current_glob_seg].end;
                        }
                        save_stat();
                    } else {
                        // Met '*'
                        ++p;
                        wild_mode = false;
                        if(p == seg_end) {
                            while(s != s_end && *s != '/') {
                                ++s;
                            }
                            if(s == s_end) {
                                continue;
                            }
                            if(s + 1 != s_end) {
                                ++s;
                            }
                            if(current_glob_seg + 1 == seg_num) {
                                return true;
                            }
                            ++current_glob_seg;
                            p = p_start + glob_segments[current_glob_seg].start;
                            seg_start = p;
                            seg_end = p_start + glob_segments[current_glob_seg].end;
                        }
                        save_stat();
                    }
                    continue;
                }

                case '?': {
                    if(p + 1 != seg_end && *(p + 1) == '*') {
                        // Handle '?*'
                        unsigned offset = *s == '\\' ? 2 : 1;
                        s += offset;
                        save_stat();
                        p += 2;
                        continue;
                    }
                    if(s != s_end && *s != '/') {
                        ++p;
                        s = *s == '\\' ? s + 2 : s + 1;
                        continue;
                    }
                    break;
                }

                case '[': {
                    if(b < brackets.size() && brackets[b].bytes[uint8_t(*s)]) {
                        if(p == seg_start && !(s == s_start || *(s - 1) == '/')) {
                            // Segment start dismatch
                            break;
                        }
                        p = pat.data() + brackets[b].next_offset;
                        ++b;
                        ++s;
                        continue;
                    }
                    break;
                }

                case '\\': {
                    if(p + 1 != seg_end && *(p + 1) == *s) {
                        if(p == seg_start && !(s == s_start || *(s - 1) == '/')) {
                            // Segment start dismatch
                            break;
                        }
                        p += 2;
                        ++s;
                        continue;
                    }
                    break;
                }

                default: {
                    if(*p == *s) {
                        if(p == seg_start && !(s == s_start || *(s - 1) == '/')) {
                            // Segment start dismatch
                            break;
                        }
                        ++p;
                        ++s;
                        continue;
                    }
                    break;
                }
            }

        } else {
            // P comes to a segment end
            if(seg_end != p_end) {
                if(wild_mode) {
                    // Step to next segment
                    ++current_glob_seg;
                    while(s != s_end && *s != '/') {
                        ++s;
                    }
                    if(s != s_end && *s == '/') {
                        ++s;
                    }
                    if(current_glob_seg >= seg_num) {
                        return s == s_end;
                    }
                    p = p_start + glob_segments[current_glob_seg].start;
                    seg_start = p;
                    seg_end = p_start + glob_segments[current_glob_seg].end;
                    continue;
                } else {
                    if(*seg_end != *s) {
                        return false;
                    }
                    // *S and *SegEnd should be '/'
                    // Escape repeat '/'
                    while(s != s_end && *s == '/') {
                        ++s;
                    }
                    // Step to next segment
                    ++current_glob_seg;
                    p = p_start + glob_segments[current_glob_seg].start;
                    seg_start = p;
                    seg_end = p_start + glob_segments[current_glob_seg].end;
                    continue;
                }
            }
        }

        if(backtrace_stack.empty()) {
            return false;
        }

        // Backstrace
        auto& state = backtrace_stack.back();

        p = state.p;
        s = ++state.s;
        b = state.b;
        current_glob_seg = state.glob_seg;
        wild_mode = state.wild_mode;
        seg_start = state.seg_start;
        seg_end = state.seg_end;

        // In non-WildMode, pop back invalid stat in the stack
        if(!wild_mode && (s == s_end || *s == '/')) {
            backtrace_stack.pop_back();
            continue;
        }

        // In non-WildMode, '/' is a barrier
        if(!wild_mode && *s == '/') {
            return false;
        }
    }

    return s == s_end;
}

}  // namespace clice
