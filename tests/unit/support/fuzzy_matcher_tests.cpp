#include <initializer_list>
#include <string>

#include "test/test.h"
#include "support/fuzzy_matcher.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

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

TEST_SUITE(FuzzyMatcher) {

TEST_CASE(Segmentation) {
    EXPECT_EQ(segmented("std::basic_string"), "+--  +---- +-----");
    EXPECT_EQ(segmented("XMLHttpRequest"), "+--+---+------");
    EXPECT_EQ(segmented("t3h PeNgU1N oF d00m!!!!!!!!"), "+-- +-+-+-+ ++ +---        ");
    EXPECT_EQ(segmented("ab\xF0\x9F\x99\x82"
                        "cd"),
              "+-------");
    EXPECT_EQ(segmented("HTMLElement"), "+---+------");
}

TEST_CASE(Accepts) {
    EXPECT_EQ(annotated("", "unique_ptr"), "unique_ptr");
    EXPECT_EQ(annotated("u_p", "unique_ptr"), "[u]nique[_p]tr");
    EXPECT_EQ(annotated("up", "unique_ptr"), "[u]nique_[p]tr");
    EXPECT_FALSE(matches("uq", "unique_ptr"));
    EXPECT_FALSE(matches("qp", "unique_ptr"));
    EXPECT_EQ(annotated("tit", "win.tit"), "win.[tit]");
    EXPECT_EQ(annotated("title", "win.title"), "win.[title]");
    EXPECT_EQ(annotated("WordCla", "WordCharacterClassifier"), "[Word]Character[Cla]ssifier");
    EXPECT_EQ(annotated("WordCCla", "WordCharacterClassifier"), "[WordC]haracter[Cla]ssifier");
    EXPECT_FALSE(matches("dete", "editor.quickSuggestionsDelay"));
    EXPECT_EQ(annotated("highlight", "editorHoverHighlight"), "editorHover[Highlight]");
    EXPECT_EQ(annotated("hhighlight", "editorHoverHighlight"), "editor[H]over[Highlight]");
    EXPECT_FALSE(matches("dhhighlight", "editorHoverHighlight"));
    EXPECT_EQ(annotated("-moz", "-moz-foo"), "[-moz]-foo");
    EXPECT_EQ(annotated("moz", "-moz-foo"), "-[moz]-foo");
    EXPECT_EQ(annotated("moza", "-moz-animation"), "-[moz]-[a]nimation");
    EXPECT_EQ(annotated("ab", "abA"), "[ab]A");
    EXPECT_FALSE(matches("ccm", "cacmelCase"));
    EXPECT_FALSE(matches("bti", "the_black_knight"));
    EXPECT_FALSE(matches("ccm", "camelCase"));
    EXPECT_FALSE(matches("cmcm", "camelCase"));
    EXPECT_EQ(annotated("BK", "the_black_knight"), "the_[b]lack_[k]night");
    EXPECT_FALSE(matches("KeyboardLayout=", "KeyboardLayout"));
    EXPECT_EQ(annotated("LLL", "SVisualLoggerLogsList"), "SVisual[L]ogger[L]ogs[L]ist");
    EXPECT_EQ(annotated("TEdit", "TextEdit"), "[T]ext[Edit]");
    EXPECT_EQ(annotated("TEdit", "TextEditor"), "[T]ext[Edit]or");
    EXPECT_FALSE(matches("TEdit", "Textedit"));
    EXPECT_EQ(annotated("TEdit", "text_edit"), "[t]ext_[edit]");
    EXPECT_EQ(annotated("TEditDt", "TextEditorDecorationType"), "[T]ext[Edit]or[D]ecoration[T]ype");
    EXPECT_EQ(annotated("Tedit", "TextEdit"), "[T]ext[Edit]");
    EXPECT_FALSE(matches("ba", "?AB?"));
    EXPECT_EQ(annotated("bkn", "the_black_knight"), "the_[b]lack_[kn]ight");
    EXPECT_FALSE(matches("bt", "the_black_knight"));
    EXPECT_FALSE(matches("fdm", "findModel"));
    EXPECT_FALSE(matches("fob", "foobar"));
    EXPECT_FALSE(matches("fobz", "foobar"));
    EXPECT_EQ(annotated("foobar", "foobar"), "[foobar]");
    EXPECT_EQ(annotated("form", "editor.formatOnSave"), "editor.[form]atOnSave");
    EXPECT_EQ(annotated("g p", "Git: Pull"), "[G]it:[ P]ull");
    EXPECT_EQ(annotated("gip", "Git: Pull"), "[Gi]t: [P]ull");
    EXPECT_EQ(annotated("gp", "Git: Pull"), "[G]it: [P]ull");
    EXPECT_EQ(annotated("gp", "Git_Git_Pull"), "[G]it_Git_[P]ull");
    EXPECT_EQ(annotated("is", "ImportStatement"), "[I]mport[S]tatement");
    EXPECT_EQ(annotated("is", "isValid"), "[is]Valid");
    EXPECT_FALSE(matches("lowrd", "lowWord"));
    EXPECT_FALSE(matches("myvable", "myvariable"));
    EXPECT_FALSE(matches("no", ""));
    EXPECT_FALSE(matches("no", "match"));
    EXPECT_EQ(annotated("sl", "SVisualLoggerLogsList"), "[S]Visual[L]oggerLogsList");
    EXPECT_EQ(annotated("sllll", "SVisualLoggerLlamaList"), "[S]Visual[L]ogger[Ll]ama[L]ist");
    EXPECT_EQ(annotated("THRE", "HTMLHRElement"), "H[T]ML[HRE]lement");
    EXPECT_EQ(annotated("Three", "Three"), "[Three]");
    EXPECT_EQ(annotated("fo", "bar_foo"), "bar_[fo]o");
    EXPECT_EQ(annotated("fo", "bar_Foo"), "bar_[Fo]o");
    EXPECT_EQ(annotated("fo", "bar foo"), "bar [fo]o");
    EXPECT_EQ(annotated("fo", "bar.foo"), "bar.[fo]o");
    EXPECT_EQ(annotated("aaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              "[aaaaaa]aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    EXPECT_FALSE(matches("fsfsfs", "dsafdsafdsafdsafdsafdsafdsafasdfdsa"));
    EXPECT_FALSE(matches("fsfsfsfsfsfsfsf",
                         "dsafdsafdsafdsafdsafdsafdsafasdfdsafdsafdsafdsafdsfd"
                         "safdsfdfdfasdnfdsajfndsjnafjndsajlknfdsa"));
    EXPECT_EQ(annotated("  g", "  group"), "[  g]roup");
    EXPECT_EQ(annotated("g", "  group"), "  [g]roup");
    EXPECT_FALSE(matches("g g", "  groupGroup"));
    EXPECT_EQ(annotated("g g", "  group Group"), "  [g]roup[ G]roup");
    EXPECT_EQ(annotated(" g g", "  group Group"), "[ ] [g]roup[ G]roup");
    EXPECT_EQ(annotated("zz", "zzGroup"), "[zz]Group");
    EXPECT_EQ(annotated("zzg", "zzGroup"), "[zzG]roup");
    EXPECT_EQ(annotated("g", "zzGroup"), "zz[G]roup");
    EXPECT_EQ(annotated("aaaa", "_a_aaaa"), "_a_[aaaa]");
    EXPECT_FALSE(matches("strcpy", "strncpy"));
    EXPECT_FALSE(matches("std", "PTHREAD_MUTEX_STALLED"));
    EXPECT_FALSE(matches("std", "pthread_condattr_setpshared"));
}

TEST_CASE(StartsInsideWord) {
    // With the option, a run may begin anywhere in a name at a low
    // score, which keeps the C library's unsegmented names reachable.
    MatchOptions inside{.inside_word = true};
    EXPECT_EQ(annotated("printf", "sprintf", inside), "s[printf]");
    EXPECT_EQ(annotated("str", "ostream", inside), "o[str]eam");
    EXPECT_EQ(annotated("fo", "barfoo", inside), "bar[fo]o");
    EXPECT_EQ(annotated("b", "NDEBUG", inside), "NDE[B]UG");
    EXPECT_EQ(annotated("baba", "ababababab", inside), "a[baba]babab");
    EXPECT_EQ(annotated("log", "SVGFEMorphologyElement", inside), "SVGFEMorpho[log]yElement");
    EXPECT_EQ(annotated("TEdit", "Textedit", inside), "Tex[tedit]");
    EXPECT_EQ(annotated("tru", "struct", inside), "s[tru]ct");
    // Only the first character may: after a gap the run lands on a head
    // or continues an initialism, and the run must finish its word.
    EXPECT_FALSE(matches("getfoo", "get_my_xfoo", inside));
    EXPECT_FALSE(matches("qp", "unique_ptr", inside));
    EXPECT_EQ(annotated("getfoo", "get_my_foo", inside), "[get]_my_[foo]");
    // Without it, the first character starts a word too.
    EXPECT_FALSE(matches("printf", "sprintf"));
    EXPECT_FALSE(matches("tru", "struct"));
    EXPECT_FALSE(matches("no", "alignof"));
    EXPECT_FALSE(matches("fo", "barfoo"));
}

TEST_CASE(Ranks) {
    EXPECT_TRUE(ranks("cons", {"console", "Console", "ArrayBufferConstructor"}));
    EXPECT_TRUE(ranks("foo", {"foo", "Foo"}));
    EXPECT_TRUE(ranks("onMes", {"onMessage", "onmessage", "onThisMegaEscapes"}));
    EXPECT_TRUE(ranks("onmes", {"onmessage", "onMessage", "onThisMegaEscapes"}));
    EXPECT_TRUE(ranks("CC", {"CamelCase", "camelCase"}));
    EXPECT_TRUE(ranks("cC", {"camelCase", "CamelCase"}));
    EXPECT_TRUE(ranks("p", {"p", "parse", "posix", "pafdsa", "path"}));
    EXPECT_TRUE(ranks("pa", {"parse", "path", "pafdsa"}));
    EXPECT_TRUE(ranks("log", {"log", "ScrollLogicalPosition"}));
    EXPECT_TRUE(ranks("e", {"else", "AbstractElement"}));
    EXPECT_TRUE(ranks("workbench.sideb",
                      {"workbench.sideBar.location", "workbench.editor.defaultSideBySideLayout"}));
    EXPECT_TRUE(ranks("editor.r",
                      {"editor.renderControlCharacter",
                       "editor.overviewRulerlanes",
                       "diffEditor.renderSideBySide"}));
    EXPECT_TRUE(ranks("-mo", {"-moz-columns", "-ms-ime-mode"}));
    EXPECT_TRUE(ranks("convertModelPosition",
                      {"convertModelPositionToViewPosition", "convertViewToModelPosition"}));
    EXPECT_TRUE(ranks("is", {"isValidViewletId", "import statement"}));
    EXPECT_TRUE(ranks("strcpy", {"strcpy", "strcpy_s"}));
    EXPECT_TRUE(ranks("foo", {"foo", "foobar", "bar_foo", "xfoo"}));
    EXPECT_TRUE(ranks("print", {"printf", "vprintf"}));
    EXPECT_TRUE(ranks("up", {"upper_bound", "unique_ptr"}));
    EXPECT_TRUE(ranks("log", {"log", "Logger", "ScrollLogicalPosition", "SVGFEMorphologyElement"}));
    EXPECT_TRUE(ranks("s", {"s", "size", "Size", "as", "less"}));
}

TEST_CASE(Scores) {
    EXPECT_EQ(score("abs", "absl"), 1.0f);
    EXPECT_EQ(score("abs", "abs"), 2.0f);
    EXPECT_GT(score("Abs", "abs"), 1.0f);
    EXPECT_GT(score("abs", "awBxYzS"), 0.0f);
    EXPECT_LT(score("abs", "awBxYzS"), 1.0f);
    EXPECT_EQ(score("", "anything"), 1.0f);
    EXPECT_EQ(score("up", "upper_bound"), 1.0f);
    EXPECT_GT(score("up", "unique_ptr"), 0.5f);
    EXPECT_LT(score("up", "unique_ptr"), 1.0f);
}

TEST_CASE(Bounds) {
    // Past the bounds a match stays partial: neither a pattern nor a
    // name longer than the bound is ever "the whole name".
    std::string sixty_three(63, 'a');
    std::string sixty_four(64, 'a');
    std::string long_name(200, 'a');
    EXPECT_EQ(score(sixty_three, sixty_three), 2.0f);
    EXPECT_LE(score(sixty_four, sixty_three), 1.0f);
    EXPECT_LE(score(std::string(127, 'a'), long_name), 1.0f);
    EXPECT_TRUE(matches(sixty_four, long_name));
    // Tokens stop at the bound too: a long name's tail is not keyed,
    // which the index makes up for by scanning such names.
    auto tail = tokens_of(long_name + "xyz");
    EXPECT_FALSE(llvm::is_contained(tail, token("xyz")));
    EXPECT_FALSE(tail.empty());
}

TEST_CASE(Typos) {
    MatchOptions typo{.typo = true, .inside_word = true};
    EXPECT_FALSE(matches("strcpy", "strncpy"));
    EXPECT_EQ(annotated("strcpy", "strncpy", typo), "[str]n[cpy]");
    EXPECT_EQ(annotated("strdpy", "strcpy", typo), "[str]c[py]");
    EXPECT_EQ(annotated("strxcpy", "strcpy", typo), "[strcpy]");
    EXPECT_EQ(annotated("strpy", "strcpy", typo), "[str]c[py]");
    EXPECT_FALSE(matches("strxxcpy", "strcpy", typo));
    EXPECT_FALSE(matches("stxxpy", "strcpy", typo));
    FuzzyMatcher lenient("strcpy", typo);
    auto edited = lenient.match("strncpy");
    EXPECT_TRUE(edited.has_value() && *edited <= 0.5f);
    // A clean match is scored as without the allowance.
    EXPECT_EQ(lenient.match("strcpy").value_or(-1), score("strcpy", "strcpy"));
    EXPECT_EQ(lenient.match("strcpy_s").value_or(-1), score("strcpy", "strcpy_s"));
}

TEST_CASE(NameTokens) {
    auto tokens = tokens_of("unique_ptr");
    EXPECT_TRUE(llvm::is_contained(tokens, token("uni")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("unp")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("upt")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("ptr")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("u")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("un")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("up")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("p")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("pt")));
    EXPECT_FALSE(llvm::is_contained(tokens, token("n")));
    EXPECT_FALSE(llvm::is_contained(tokens, token("uq")));
    EXPECT_FALSE(llvm::is_contained(tokens, token("nqp")));
    EXPECT_TRUE(llvm::is_sorted(tokens));
    EXPECT_TRUE(tokens_of("").empty());
    EXPECT_TRUE(tokens_of("__").empty());
    // Short tokens come from the first two heads only.
    auto three = tokens_of("foo_bar_baz");
    EXPECT_TRUE(llvm::is_contained(three, token("fb")));
    EXPECT_TRUE(llvm::is_contained(three, token("b")));
    EXPECT_TRUE(llvm::is_contained(three, token("bb")));
    EXPECT_FALSE(llvm::is_contained(three, token("bz")));
}

TEST_CASE(QueryTokens) {
    llvm::SmallVector<NameToken> tokens;
    query_tokens("getFoo", tokens);
    EXPECT_EQ(tokens.size(), std::size_t(4));
    EXPECT_TRUE(llvm::is_contained(tokens, token("get")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("etf")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("tfo")));
    EXPECT_TRUE(llvm::is_contained(tokens, token("foo")));
    query_tokens("u_p", tokens);
    EXPECT_EQ(tokens.size(), std::size_t(1));
    EXPECT_EQ(tokens.front(), token("up"));
    query_tokens("X", tokens);
    EXPECT_EQ(tokens.front(), token("x"));
    query_tokens("::", tokens);
    EXPECT_TRUE(tokens.empty());

    llvm::SmallVector<TypoAlternative> alternatives;
    typo_tokens("strcp", alternatives);
    EXPECT_TRUE(alternatives.empty());
    typo_tokens("strcpy", alternatives);
    EXPECT_EQ(alternatives.size(), std::size_t(6));
    // Wrong third letter: nothing survives before it, `cpy` after.
    EXPECT_EQ(alternatives[2].tokens.size(), std::size_t(1));
    EXPECT_EQ(alternatives[2].tokens.front(), token("cpy"));
    for(auto& alternative: alternatives) {
        EXPECT_FALSE(alternative.tokens.empty());
    }
}

/// A name the matcher accepts carries every token of the pattern — the
/// property that makes token intersection a complete retrieval. Checked
/// over every three-to-five letter subsequence of the corpus names, plus
/// their one-edit corruptions against the typo alternatives.
TEST_CASE(TokensCoverMatches) {
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
    EXPECT_EQ(uncovered, "");
    EXPECT_GT(accepted, std::size_t(100));
    EXPECT_GT(typo_accepted, std::size_t(100));
}

};  // TEST_SUITE(FuzzyMatcher)

}  // namespace
}  // namespace clice::testing
