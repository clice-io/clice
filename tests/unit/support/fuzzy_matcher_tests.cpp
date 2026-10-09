module;

#include "modules/prelude.h"

module clice;

import :support.fuzzy_matcher;
import :tests.unit.test.test;

namespace clice::testing {
namespace {

std::string annotated(llvm::StringRef pattern, llvm::StringRef name, MatchOptions options = {}) {
    FuzzyMatcher matcher(pattern, options);
    return matcher.annotate(name);
}

bool matches(llvm::StringRef pattern, llvm::StringRef name, MatchOptions options = {}) {
    FuzzyMatcher matcher(pattern, options);
    return matcher.match(name).has_value();
}

float score(llvm::StringRef pattern, llvm::StringRef name) {
    FuzzyMatcher matcher(pattern);
    return matcher.match(name).value_or(-1);
}

/// Whether every name matches — a run allowed to start inside a word,
/// as a search ranks — and their scores never rise along the list.
bool ranks(llvm::StringRef pattern, std::initializer_list<llvm::StringRef> names) {
    FuzzyMatcher matcher(pattern, {.inside_word = true});
    float last = 3;
    for(auto name: names) {
        auto current = matcher.match(name);
        if(!current || *current > last) {
            return false;
        }
        last = *current;
    }
    return true;
}

/// Roles rendered one character each: `+` head, `-` tail, space separator.
std::string segmented(llvm::StringRef text) {
    llvm::SmallVector<CharRole> roles(text.size());
    segment(text, roles);
    std::string out;
    for(auto role: roles) {
        out += role == CharRole::Head ? '+' : role == CharRole::Tail ? '-' : ' ';
    }
    return out;
}

NameToken token(llvm::StringRef text) {
    NameToken value = 0;
    for(char c: text) {
        value = (value << 8) | static_cast<unsigned char>(c);
    }
    return value;
}

llvm::SmallVector<NameToken> tokens_of(llvm::StringRef name) {
    llvm::SmallVector<NameToken> tokens;
    name_tokens(name, tokens);
    return tokens;
}

bool subset(llvm::ArrayRef<NameToken> part, llvm::ArrayRef<NameToken> whole) {
    return llvm::all_of(part, [&](NameToken t) { return llvm::is_contained(whole, t); });
}

/// The letters of `text` (its non-separators, as the tokens see them).
std::string letters(llvm::StringRef text) {
    std::string out;
    llvm::SmallVector<CharRole> roles(text.size());
    segment(text, roles);
    for(std::size_t i = 0; i < text.size(); i += 1) {
        if(roles[i] != CharRole::Separator) {
            out += text[i];
        }
    }
    return out;
}

std::size_t choose(std::size_t n, std::size_t k) {
    std::size_t result = 1;
    for(std::size_t i = 1; i <= k && i <= n; i += 1) {
        result = result * (n - k + i) / i;
    }
    return k > n ? 0 : result;
}

/// About `budget` subsequences of `text` with `length` characters, spread
/// evenly over all of them: enough for a property, cheap enough for a
/// routine run under a sanitizer.
void subsequences(llvm::StringRef text,
                  std::size_t length,
                  std::size_t budget,
                  llvm::function_ref<void(llvm::StringRef)> visit) {
    std::size_t stride = std::max<std::size_t>(1, choose(text.size(), length) / budget);
    std::size_t seen = 0;
    std::string current;
    auto recurse = [&](auto& self, std::size_t from) -> void {
        if(current.size() == length) {
            seen += 1;
            if(seen % stride == 0) {
                visit(current);
            }
            return;
        }
        for(std::size_t i = from; i + (length - current.size()) <= text.size(); i += 1) {
            current += text[i];
            self(self, i + 1);
            current.pop_back();
        }
    };
    recurse(recurse, 0);
}

constexpr llvm::StringRef corpus[] = {
    "unique_ptr",
    "XMLHttpRequest",
    "HTMLElement",
    "vsprintf",
    "the_black_knight",
    "SVisualLoggerLogsList",
    "foo_bar_baz",
    "NDEBUG",
    "editorHoverHighlight",
    "MAX_SIZE",
    "a1b2c3",
    "basic_string",
    "operator<<",
    "~Foo",
    "__builtin_expect",
    "m_fooBar",
    "getFooBarBaz",
    "ab\xF0\x9F\x99\x82"
    "cd",
    "PTHREAD_MUTEX_STALLED",
    "convertModelPosition",
};

ZEST_SUITE(FuzzyMatcher) {

ZEST_CASE(Segmentation) {
    ZEXPECT(segmented("std::basic_string") == "+--  +---- +-----");
    ZEXPECT(segmented("XMLHttpRequest") == "+--+---+------");
    ZEXPECT(segmented("t3h PeNgU1N oF d00m!!!!!!!!") == "+-- +-+-+-+ ++ +---        ");
    ZEXPECT(segmented("ab\xF0\x9F\x99\x82"
                      "cd") == "+-------");
    ZEXPECT(segmented("HTMLElement") == "+---+------");
}

ZEST_CASE(Accepts) {
    ZEXPECT(annotated("", "unique_ptr") == "unique_ptr");
    ZEXPECT(annotated("u_p", "unique_ptr") == "[u]nique[_p]tr");
    ZEXPECT(annotated("up", "unique_ptr") == "[u]nique_[p]tr");
    ZEXPECT(!matches("uq", "unique_ptr"));
    ZEXPECT(!matches("qp", "unique_ptr"));
    ZEXPECT(annotated("tit", "win.tit") == "win.[tit]");
    ZEXPECT(annotated("title", "win.title") == "win.[title]");
    ZEXPECT(annotated("WordCla", "WordCharacterClassifier") == "[Word]Character[Cla]ssifier");
    ZEXPECT(annotated("WordCCla", "WordCharacterClassifier") == "[WordC]haracter[Cla]ssifier");
    ZEXPECT(!matches("dete", "editor.quickSuggestionsDelay"));
    ZEXPECT(annotated("highlight", "editorHoverHighlight") == "editorHover[Highlight]");
    ZEXPECT(annotated("hhighlight", "editorHoverHighlight") == "editor[H]over[Highlight]");
    ZEXPECT(!matches("dhhighlight", "editorHoverHighlight"));
    ZEXPECT(annotated("-moz", "-moz-foo") == "[-moz]-foo");
    ZEXPECT(annotated("moz", "-moz-foo") == "-[moz]-foo");
    ZEXPECT(annotated("moza", "-moz-animation") == "-[moz]-[a]nimation");
    ZEXPECT(annotated("ab", "abA") == "[ab]A");
    ZEXPECT(!matches("ccm", "cacmelCase"));
    ZEXPECT(!matches("bti", "the_black_knight"));
    ZEXPECT(!matches("ccm", "camelCase"));
    ZEXPECT(!matches("cmcm", "camelCase"));
    ZEXPECT(annotated("BK", "the_black_knight") == "the_[b]lack_[k]night");
    ZEXPECT(!matches("KeyboardLayout=", "KeyboardLayout"));
    ZEXPECT(annotated("LLL", "SVisualLoggerLogsList") == "SVisual[L]ogger[L]ogs[L]ist");
    ZEXPECT(annotated("TEdit", "TextEdit") == "[T]ext[Edit]");
    ZEXPECT(annotated("TEdit", "TextEditor") == "[T]ext[Edit]or");
    ZEXPECT(!matches("TEdit", "Textedit"));
    ZEXPECT(annotated("TEdit", "text_edit") == "[t]ext_[edit]");
    ZEXPECT(annotated("TEditDt", "TextEditorDecorationType") == "[T]ext[Edit]or[D]ecoration[T]ype");
    ZEXPECT(annotated("Tedit", "TextEdit") == "[T]ext[Edit]");
    ZEXPECT(!matches("ba", "?AB?"));
    ZEXPECT(annotated("bkn", "the_black_knight") == "the_[b]lack_[kn]ight");
    ZEXPECT(!matches("bt", "the_black_knight"));
    ZEXPECT(!matches("fdm", "findModel"));
    ZEXPECT(!matches("fob", "foobar"));
    ZEXPECT(!matches("fobz", "foobar"));
    ZEXPECT(annotated("foobar", "foobar") == "[foobar]");
    ZEXPECT(annotated("form", "editor.formatOnSave") == "editor.[form]atOnSave");
    ZEXPECT(annotated("g p", "Git: Pull") == "[G]it:[ P]ull");
    ZEXPECT(annotated("gip", "Git: Pull") == "[Gi]t: [P]ull");
    ZEXPECT(annotated("gp", "Git: Pull") == "[G]it: [P]ull");
    ZEXPECT(annotated("gp", "Git_Git_Pull") == "[G]it_Git_[P]ull");
    ZEXPECT(annotated("is", "ImportStatement") == "[I]mport[S]tatement");
    ZEXPECT(annotated("is", "isValid") == "[is]Valid");
    ZEXPECT(!matches("lowrd", "lowWord"));
    ZEXPECT(!matches("myvable", "myvariable"));
    ZEXPECT(!matches("no", ""));
    ZEXPECT(!matches("no", "match"));
    ZEXPECT(annotated("sl", "SVisualLoggerLogsList") == "[S]Visual[L]oggerLogsList");
    ZEXPECT(annotated("sllll", "SVisualLoggerLlamaList") == "[S]Visual[L]ogger[Ll]ama[L]ist");
    ZEXPECT(annotated("THRE", "HTMLHRElement") == "H[T]ML[HRE]lement");
    ZEXPECT(annotated("Three", "Three") == "[Three]");
    ZEXPECT(annotated("fo", "bar_foo") == "bar_[fo]o");
    ZEXPECT(annotated("fo", "bar_Foo") == "bar_[Fo]o");
    ZEXPECT(annotated("fo", "bar foo") == "bar [fo]o");
    ZEXPECT(annotated("fo", "bar.foo") == "bar.[fo]o");
    ZEXPECT(annotated("aaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") ==
            "[aaaaaa]aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    ZEXPECT(!matches("fsfsfs", "dsafdsafdsafdsafdsafdsafdsafasdfdsa"));
    ZEXPECT(!matches("fsfsfsfsfsfsfsf",
                     "dsafdsafdsafdsafdsafdsafdsafasdfdsafdsafdsafdsafdsfd"
                     "safdsfdfdfasdnfdsajfndsjnafjndsajlknfdsa"));
    ZEXPECT(annotated("  g", "  group") == "[  g]roup");
    ZEXPECT(annotated("g", "  group") == "  [g]roup");
    ZEXPECT(!matches("g g", "  groupGroup"));
    ZEXPECT(annotated("g g", "  group Group") == "  [g]roup[ G]roup");
    ZEXPECT(annotated(" g g", "  group Group") == "[ ] [g]roup[ G]roup");
    ZEXPECT(annotated("zz", "zzGroup") == "[zz]Group");
    ZEXPECT(annotated("zzg", "zzGroup") == "[zzG]roup");
    ZEXPECT(annotated("g", "zzGroup") == "zz[G]roup");
    ZEXPECT(annotated("aaaa", "_a_aaaa") == "_a_[aaaa]");
    ZEXPECT(!matches("strcpy", "strncpy"));
    ZEXPECT(!matches("std", "PTHREAD_MUTEX_STALLED"));
    ZEXPECT(!matches("std", "pthread_condattr_setpshared"));
}

ZEST_CASE(StartsInsideWord) {
    // With the option, a run may begin anywhere in a name at a low
    // score, which keeps the C library's unsegmented names reachable.
    MatchOptions inside{.inside_word = true};
    ZEXPECT(annotated("printf", "sprintf", inside) == "s[printf]");
    ZEXPECT(annotated("str", "ostream", inside) == "o[str]eam");
    ZEXPECT(annotated("fo", "barfoo", inside) == "bar[fo]o");
    ZEXPECT(annotated("b", "NDEBUG", inside) == "NDE[B]UG");
    ZEXPECT(annotated("baba", "ababababab", inside) == "a[baba]babab");
    ZEXPECT(annotated("log", "SVGFEMorphologyElement", inside) == "SVGFEMorpho[log]yElement");
    ZEXPECT(annotated("TEdit", "Textedit", inside) == "Tex[tedit]");
    ZEXPECT(annotated("tru", "struct", inside) == "s[tru]ct");
    // Only the first character may: after a gap the run lands on a head
    // or continues an initialism, and the run must finish its word.
    ZEXPECT(!matches("getfoo", "get_my_xfoo", inside));
    ZEXPECT(!matches("qp", "unique_ptr", inside));
    ZEXPECT(annotated("getfoo", "get_my_foo", inside) == "[get]_my_[foo]");
    // Without it, the first character starts a word too.
    ZEXPECT(!matches("printf", "sprintf"));
    ZEXPECT(!matches("tru", "struct"));
    ZEXPECT(!matches("no", "alignof"));
    ZEXPECT(!matches("fo", "barfoo"));
}

ZEST_CASE(Ranks) {
    ZEXPECT(ranks("cons", {"console", "Console", "ArrayBufferConstructor"}));
    ZEXPECT(ranks("foo", {"foo", "Foo"}));
    ZEXPECT(ranks("onMes", {"onMessage", "onmessage", "onThisMegaEscapes"}));
    ZEXPECT(ranks("onmes", {"onmessage", "onMessage", "onThisMegaEscapes"}));
    ZEXPECT(ranks("CC", {"CamelCase", "camelCase"}));
    ZEXPECT(ranks("cC", {"camelCase", "CamelCase"}));
    ZEXPECT(ranks("p", {"p", "parse", "posix", "pafdsa", "path"}));
    ZEXPECT(ranks("pa", {"parse", "path", "pafdsa"}));
    ZEXPECT(ranks("log", {"log", "ScrollLogicalPosition"}));
    ZEXPECT(ranks("e", {"else", "AbstractElement"}));
    ZEXPECT(ranks("workbench.sideb",
                  {"workbench.sideBar.location", "workbench.editor.defaultSideBySideLayout"}));
    ZEXPECT(ranks("editor.r",
                  {"editor.renderControlCharacter",
                   "editor.overviewRulerlanes",
                   "diffEditor.renderSideBySide"}));
    ZEXPECT(ranks("-mo", {"-moz-columns", "-ms-ime-mode"}));
    ZEXPECT(ranks("convertModelPosition",
                  {"convertModelPositionToViewPosition", "convertViewToModelPosition"}));
    ZEXPECT(ranks("is", {"isValidViewletId", "import statement"}));
    ZEXPECT(ranks("strcpy", {"strcpy", "strcpy_s"}));
    ZEXPECT(ranks("foo", {"foo", "foobar", "bar_foo", "xfoo"}));
    ZEXPECT(ranks("print", {"printf", "vprintf"}));
    ZEXPECT(ranks("up", {"upper_bound", "unique_ptr"}));
    ZEXPECT(ranks("log", {"log", "Logger", "ScrollLogicalPosition", "SVGFEMorphologyElement"}));
    ZEXPECT(ranks("s", {"s", "size", "Size", "as", "less"}));
}

ZEST_CASE(Scores) {
    ZEXPECT(score("abs", "absl") == 1.0f);
    ZEXPECT(score("abs", "abs") == 2.0f);
    ZEXPECT(score("Abs", "abs") > 1.0f);
    ZEXPECT(score("abs", "awBxYzS") > 0.0f);
    ZEXPECT(score("abs", "awBxYzS") < 1.0f);
    ZEXPECT(score("", "anything") == 1.0f);
    ZEXPECT(score("up", "upper_bound") == 1.0f);
    ZEXPECT(score("up", "unique_ptr") > 0.5f);
    ZEXPECT(score("up", "unique_ptr") < 1.0f);
}

ZEST_CASE(Bounds) {
    // Past the bounds a match stays partial: neither a pattern nor a
    // name longer than the bound is ever "the whole name".
    std::string sixty_three(63, 'a');
    std::string sixty_four(64, 'a');
    std::string long_name(200, 'a');
    ZEXPECT(score(sixty_three, sixty_three) == 2.0f);
    ZEXPECT(score(sixty_four, sixty_three) <= 1.0f);
    ZEXPECT(score(std::string(127, 'a'), long_name) <= 1.0f);
    ZEXPECT(matches(sixty_four, long_name));
    // Tokens stop at the bound too: a long name's tail is not keyed,
    // which the index makes up for by scanning such names.
    auto tail = tokens_of(long_name + "xyz");
    ZEXPECT(!llvm::is_contained(tail, token("xyz")));
    ZEXPECT(!tail.empty());
}

ZEST_CASE(Typos) {
    MatchOptions typo{.typo = true, .inside_word = true};
    ZEXPECT(!matches("strcpy", "strncpy"));
    ZEXPECT(annotated("strcpy", "strncpy", typo) == "[str]n[cpy]");
    ZEXPECT(annotated("strdpy", "strcpy", typo) == "[str]c[py]");
    ZEXPECT(annotated("strxcpy", "strcpy", typo) == "[strcpy]");
    ZEXPECT(annotated("strpy", "strcpy", typo) == "[str]c[py]");
    ZEXPECT(!matches("strxxcpy", "strcpy", typo));
    ZEXPECT(!matches("stxxpy", "strcpy", typo));
    FuzzyMatcher lenient("strcpy", typo);
    auto edited = lenient.match("strncpy");
    ZEXPECT((edited.has_value() && *edited <= 0.5f));
    // A clean match is scored as without the allowance.
    ZEXPECT(lenient.match("strcpy").value_or(-1) == score("strcpy", "strcpy"));
    ZEXPECT(lenient.match("strcpy_s").value_or(-1) == score("strcpy", "strcpy_s"));
}

ZEST_CASE(NameTokens) {
    auto tokens = tokens_of("unique_ptr");
    ZEXPECT(llvm::is_contained(tokens, token("uni")));
    ZEXPECT(llvm::is_contained(tokens, token("unp")));
    ZEXPECT(llvm::is_contained(tokens, token("upt")));
    ZEXPECT(llvm::is_contained(tokens, token("ptr")));
    ZEXPECT(llvm::is_contained(tokens, token("u")));
    ZEXPECT(llvm::is_contained(tokens, token("un")));
    ZEXPECT(llvm::is_contained(tokens, token("up")));
    ZEXPECT(llvm::is_contained(tokens, token("p")));
    ZEXPECT(llvm::is_contained(tokens, token("pt")));
    ZEXPECT(!llvm::is_contained(tokens, token("n")));
    ZEXPECT(!llvm::is_contained(tokens, token("uq")));
    ZEXPECT(!llvm::is_contained(tokens, token("nqp")));
    ZEXPECT(llvm::is_sorted(tokens));
    ZEXPECT(tokens_of("").empty());
    ZEXPECT(tokens_of("__").empty());
    // Short tokens come from the first two heads only.
    auto three = tokens_of("foo_bar_baz");
    ZEXPECT(llvm::is_contained(three, token("fb")));
    ZEXPECT(llvm::is_contained(three, token("b")));
    ZEXPECT(llvm::is_contained(three, token("bb")));
    ZEXPECT(!llvm::is_contained(three, token("bz")));
}

ZEST_CASE(QueryTokens) {
    llvm::SmallVector<NameToken> tokens;
    query_tokens("getFoo", tokens);
    ZEXPECT(tokens.size() == std::size_t(4));
    ZEXPECT(llvm::is_contained(tokens, token("get")));
    ZEXPECT(llvm::is_contained(tokens, token("etf")));
    ZEXPECT(llvm::is_contained(tokens, token("tfo")));
    ZEXPECT(llvm::is_contained(tokens, token("foo")));
    query_tokens("u_p", tokens);
    ZEXPECT(tokens.size() == std::size_t(1));
    ZEXPECT(tokens.front() == token("up"));
    query_tokens("X", tokens);
    ZEXPECT(tokens.front() == token("x"));
    query_tokens("::", tokens);
    ZEXPECT(tokens.empty());

    llvm::SmallVector<TypoAlternative> alternatives;
    typo_tokens("strcp", alternatives);
    ZEXPECT(alternatives.empty());
    typo_tokens("strcpy", alternatives);
    ZEXPECT(alternatives.size() == std::size_t(6));
    // Wrong third letter: nothing survives before it, `cpy` after.
    ZEXPECT(alternatives[2].tokens.size() == std::size_t(1));
    ZEXPECT(alternatives[2].tokens.front() == token("cpy"));
    for(auto& alternative: alternatives) {
        ZEXPECT(!alternative.tokens.empty());
    }
}

/// A name the matcher accepts carries every token of the pattern — the
/// property that makes token intersection a complete retrieval. Checked
/// over every three-to-five letter subsequence of the corpus names, plus
/// their one-edit corruptions against the typo alternatives.
ZEST_CASE(TokensCoverMatches) {
    std::size_t accepted = 0;
    std::size_t typo_accepted = 0;
    std::string uncovered;
    for(auto name: corpus) {
        auto name_keys = tokens_of(name);
        auto pool = letters(name);
        for(std::size_t length = 3; length <= 5 && length <= pool.size(); length += 1) {
            subsequences(pool, length, 60, [&](llvm::StringRef pattern) {
                if(!matches(pattern, name, {.inside_word = true})) {
                    return;
                }
                accepted += 1;
                llvm::SmallVector<NameToken> keys;
                query_tokens(pattern, keys);
                if(!subset(keys, name_keys) && uncovered.empty()) {
                    uncovered = pattern.str() + " in " + name.str();
                }
            });
        }
        if(pool.size() < 6) {
            continue;
        }
        subsequences(pool, 6, 30, [&](llvm::StringRef base) {
            for(std::size_t at = 0; at < base.size(); at += 1) {
                std::string replaced = base.str();
                replaced[at] = 'z';
                std::string dropped = base.str();
                dropped.erase(at, 1);
                std::string inserted = base.str();
                inserted.insert(at, 1, 'z');
                for(auto& pattern: {replaced, dropped, inserted}) {
                    if(!matches(pattern, name, {.typo = true, .inside_word = true})) {
                        continue;
                    }
                    typo_accepted += 1;
                    llvm::SmallVector<TypoAlternative> alternatives;
                    typo_tokens(pattern, alternatives);
                    bool covered = alternatives.empty() ||
                                   llvm::any_of(alternatives, [&](const TypoAlternative& a) {
                                       return subset(a.tokens, name_keys);
                                   });
                    if(!covered && uncovered.empty()) {
                        uncovered = pattern + " in " + name.str();
                    }
                }
            }
        });
    }
    ZEXPECT(uncovered == "");
    ZEXPECT(accepted > std::size_t(100));
    ZEXPECT(typo_accepted > std::size_t(100));
}

};  // ZEST_SUITE(FuzzyMatcher)

}  // namespace
}  // namespace clice::testing
