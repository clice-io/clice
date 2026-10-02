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

ZEST_SUITE(FuzzyMatcher){

    ZEST_CASE(Segmentation){EXPECT(segmented("std::basic_string") == "+--  +---- +-----");
EXPECT(segmented("XMLHttpRequest") == "+--+---+------");
EXPECT(segmented("t3h PeNgU1N oF d00m!!!!!!!!") == "+-- +-+-+-+ ++ +---        ");
EXPECT(segmented("ab\xF0\x9F\x99\x82"
                 "cd") == "+-------");
EXPECT(segmented("HTMLElement") == "+---+------");

}  // namespace

ZEST_CASE(Accepts) {
    EXPECT(annotated("", "unique_ptr") == "unique_ptr");
    EXPECT(annotated("u_p", "unique_ptr") == "[u]nique[_p]tr");
    EXPECT(annotated("up", "unique_ptr") == "[u]nique_[p]tr");
    EXPECT(!matches("uq", "unique_ptr"));
    EXPECT(!matches("qp", "unique_ptr"));
    EXPECT(annotated("tit", "win.tit") == "win.[tit]");
    EXPECT(annotated("title", "win.title") == "win.[title]");
    EXPECT(annotated("WordCla", "WordCharacterClassifier") == "[Word]Character[Cla]ssifier");
    EXPECT(annotated("WordCCla", "WordCharacterClassifier") == "[WordC]haracter[Cla]ssifier");
    EXPECT(!matches("dete", "editor.quickSuggestionsDelay"));
    EXPECT(annotated("highlight", "editorHoverHighlight") == "editorHover[Highlight]");
    EXPECT(annotated("hhighlight", "editorHoverHighlight") == "editor[H]over[Highlight]");
    EXPECT(!matches("dhhighlight", "editorHoverHighlight"));
    EXPECT(annotated("-moz", "-moz-foo") == "[-moz]-foo");
    EXPECT(annotated("moz", "-moz-foo") == "-[moz]-foo");
    EXPECT(annotated("moza", "-moz-animation") == "-[moz]-[a]nimation");
    EXPECT(annotated("ab", "abA") == "[ab]A");
    EXPECT(!matches("ccm", "cacmelCase"));
    EXPECT(!matches("bti", "the_black_knight"));
    EXPECT(!matches("ccm", "camelCase"));
    EXPECT(!matches("cmcm", "camelCase"));
    EXPECT(annotated("BK", "the_black_knight") == "the_[b]lack_[k]night");
    EXPECT(!matches("KeyboardLayout=", "KeyboardLayout"));
    EXPECT(annotated("LLL", "SVisualLoggerLogsList") == "SVisual[L]ogger[L]ogs[L]ist");
    EXPECT(annotated("TEdit", "TextEdit") == "[T]ext[Edit]");
    EXPECT(annotated("TEdit", "TextEditor") == "[T]ext[Edit]or");
    EXPECT(!matches("TEdit", "Textedit"));
    EXPECT(annotated("TEdit", "text_edit") == "[t]ext_[edit]");
    EXPECT(annotated("TEditDt", "TextEditorDecorationType") == "[T]ext[Edit]or[D]ecoration[T]ype");
    EXPECT(annotated("Tedit", "TextEdit") == "[T]ext[Edit]");
    EXPECT(!matches("ba", "?AB?"));
    EXPECT(annotated("bkn", "the_black_knight") == "the_[b]lack_[kn]ight");
    EXPECT(!matches("bt", "the_black_knight"));
    EXPECT(!matches("fdm", "findModel"));
    EXPECT(!matches("fob", "foobar"));
    EXPECT(!matches("fobz", "foobar"));
    EXPECT(annotated("foobar", "foobar") == "[foobar]");
    EXPECT(annotated("form", "editor.formatOnSave") == "editor.[form]atOnSave");
    EXPECT(annotated("g p", "Git: Pull") == "[G]it:[ P]ull");
    EXPECT(annotated("gip", "Git: Pull") == "[Gi]t: [P]ull");
    EXPECT(annotated("gp", "Git: Pull") == "[G]it: [P]ull");
    EXPECT(annotated("gp", "Git_Git_Pull") == "[G]it_Git_[P]ull");
    EXPECT(annotated("is", "ImportStatement") == "[I]mport[S]tatement");
    EXPECT(annotated("is", "isValid") == "[is]Valid");
    EXPECT(!matches("lowrd", "lowWord"));
    EXPECT(!matches("myvable", "myvariable"));
    EXPECT(!matches("no", ""));
    EXPECT(!matches("no", "match"));
    EXPECT(annotated("sl", "SVisualLoggerLogsList") == "[S]Visual[L]oggerLogsList");
    EXPECT(annotated("sllll", "SVisualLoggerLlamaList") == "[S]Visual[L]ogger[Ll]ama[L]ist");
    EXPECT(annotated("THRE", "HTMLHRElement") == "H[T]ML[HRE]lement");
    EXPECT(annotated("Three", "Three") == "[Three]");
    EXPECT(annotated("fo", "bar_foo") == "bar_[fo]o");
    EXPECT(annotated("fo", "bar_Foo") == "bar_[Fo]o");
    EXPECT(annotated("fo", "bar foo") == "bar [fo]o");
    EXPECT(annotated("fo", "bar.foo") == "bar.[fo]o");
    EXPECT(annotated("aaaaaa", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") ==
           "[aaaaaa]aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    EXPECT(!matches("fsfsfs", "dsafdsafdsafdsafdsafdsafdsafasdfdsa"));
    EXPECT(!matches("fsfsfsfsfsfsfsf",
                    "dsafdsafdsafdsafdsafdsafdsafasdfdsafdsafdsafdsafdsfd"
                    "safdsfdfdfasdnfdsajfndsjnafjndsajlknfdsa"));
    EXPECT(annotated("  g", "  group") == "[  g]roup");
    EXPECT(annotated("g", "  group") == "  [g]roup");
    EXPECT(!matches("g g", "  groupGroup"));
    EXPECT(annotated("g g", "  group Group") == "  [g]roup[ G]roup");
    EXPECT(annotated(" g g", "  group Group") == "[ ] [g]roup[ G]roup");
    EXPECT(annotated("zz", "zzGroup") == "[zz]Group");
    EXPECT(annotated("zzg", "zzGroup") == "[zzG]roup");
    EXPECT(annotated("g", "zzGroup") == "zz[G]roup");
    EXPECT(annotated("aaaa", "_a_aaaa") == "_a_[aaaa]");
    EXPECT(!matches("strcpy", "strncpy"));
    EXPECT(!matches("std", "PTHREAD_MUTEX_STALLED"));
    EXPECT(!matches("std", "pthread_condattr_setpshared"));
}

ZEST_CASE(StartsInsideWord) {
    // With the option, a run may begin anywhere in a name at a low
    // score, which keeps the C library's unsegmented names reachable.
    MatchOptions inside{.inside_word = true};
    EXPECT(annotated("printf", "sprintf", inside) == "s[printf]");
    EXPECT(annotated("str", "ostream", inside) == "o[str]eam");
    EXPECT(annotated("fo", "barfoo", inside) == "bar[fo]o");
    EXPECT(annotated("b", "NDEBUG", inside) == "NDE[B]UG");
    EXPECT(annotated("baba", "ababababab", inside) == "a[baba]babab");
    EXPECT(annotated("log", "SVGFEMorphologyElement", inside) == "SVGFEMorpho[log]yElement");
    EXPECT(annotated("TEdit", "Textedit", inside) == "Tex[tedit]");
    EXPECT(annotated("tru", "struct", inside) == "s[tru]ct");
    // Only the first character may: after a gap the run lands on a head
    // or continues an initialism, and the run must finish its word.
    EXPECT(!matches("getfoo", "get_my_xfoo", inside));
    EXPECT(!matches("qp", "unique_ptr", inside));
    EXPECT(annotated("getfoo", "get_my_foo", inside) == "[get]_my_[foo]");
    // Without it, the first character starts a word too.
    EXPECT(!matches("printf", "sprintf"));
    EXPECT(!matches("tru", "struct"));
    EXPECT(!matches("no", "alignof"));
    EXPECT(!matches("fo", "barfoo"));
}

ZEST_CASE(Ranks) {
    EXPECT(ranks("cons", {"console", "Console", "ArrayBufferConstructor"}));
    EXPECT(ranks("foo", {"foo", "Foo"}));
    EXPECT(ranks("onMes", {"onMessage", "onmessage", "onThisMegaEscapes"}));
    EXPECT(ranks("onmes", {"onmessage", "onMessage", "onThisMegaEscapes"}));
    EXPECT(ranks("CC", {"CamelCase", "camelCase"}));
    EXPECT(ranks("cC", {"camelCase", "CamelCase"}));
    EXPECT(ranks("p", {"p", "parse", "posix", "pafdsa", "path"}));
    EXPECT(ranks("pa", {"parse", "path", "pafdsa"}));
    EXPECT(ranks("log", {"log", "ScrollLogicalPosition"}));
    EXPECT(ranks("e", {"else", "AbstractElement"}));
    EXPECT(ranks("workbench.sideb",
                 {"workbench.sideBar.location", "workbench.editor.defaultSideBySideLayout"}));
    EXPECT(ranks("editor.r",
                 {"editor.renderControlCharacter",
                  "editor.overviewRulerlanes",
                  "diffEditor.renderSideBySide"}));
    EXPECT(ranks("-mo", {"-moz-columns", "-ms-ime-mode"}));
    EXPECT(ranks("convertModelPosition",
                 {"convertModelPositionToViewPosition", "convertViewToModelPosition"}));
    EXPECT(ranks("is", {"isValidViewletId", "import statement"}));
    EXPECT(ranks("strcpy", {"strcpy", "strcpy_s"}));
    EXPECT(ranks("foo", {"foo", "foobar", "bar_foo", "xfoo"}));
    EXPECT(ranks("print", {"printf", "vprintf"}));
    EXPECT(ranks("up", {"upper_bound", "unique_ptr"}));
    EXPECT(ranks("log", {"log", "Logger", "ScrollLogicalPosition", "SVGFEMorphologyElement"}));
    EXPECT(ranks("s", {"s", "size", "Size", "as", "less"}));
}

ZEST_CASE(Scores) {
    EXPECT(score("abs", "absl") == 1.0f);
    EXPECT(score("abs", "abs") == 2.0f);
    EXPECT(score("Abs", "abs") > 1.0f);
    EXPECT(score("abs", "awBxYzS") > 0.0f);
    EXPECT(score("abs", "awBxYzS") < 1.0f);
    EXPECT(score("", "anything") == 1.0f);
    EXPECT(score("up", "upper_bound") == 1.0f);
    EXPECT(score("up", "unique_ptr") > 0.5f);
    EXPECT(score("up", "unique_ptr") < 1.0f);
}

ZEST_CASE(Bounds) {
    // Past the bounds a match stays partial: neither a pattern nor a
    // name longer than the bound is ever "the whole name".
    std::string sixty_three(63, 'a');
    std::string sixty_four(64, 'a');
    std::string long_name(200, 'a');
    EXPECT(score(sixty_three, sixty_three) == 2.0f);
    EXPECT(score(sixty_four, sixty_three) <= 1.0f);
    EXPECT(score(std::string(127, 'a'), long_name) <= 1.0f);
    EXPECT(matches(sixty_four, long_name));
    // Tokens stop at the bound too: a long name's tail is not keyed,
    // which the index makes up for by scanning such names.
    auto tail = tokens_of(long_name + "xyz");
    EXPECT(!llvm::is_contained(tail, token("xyz")));
    EXPECT(!tail.empty());
}

ZEST_CASE(Typos) {
    MatchOptions typo{.typo = true, .inside_word = true};
    EXPECT(!matches("strcpy", "strncpy"));
    EXPECT(annotated("strcpy", "strncpy", typo) == "[str]n[cpy]");
    EXPECT(annotated("strdpy", "strcpy", typo) == "[str]c[py]");
    EXPECT(annotated("strxcpy", "strcpy", typo) == "[strcpy]");
    EXPECT(annotated("strpy", "strcpy", typo) == "[str]c[py]");
    EXPECT(!matches("strxxcpy", "strcpy", typo));
    EXPECT(!matches("stxxpy", "strcpy", typo));
    FuzzyMatcher lenient("strcpy", typo);
    auto edited = lenient.match("strncpy");
    EXPECT((edited.has_value() && *edited <= 0.5f));
    // A clean match is scored as without the allowance.
    EXPECT(lenient.match("strcpy").value_or(-1) == score("strcpy", "strcpy"));
    EXPECT(lenient.match("strcpy_s").value_or(-1) == score("strcpy", "strcpy_s"));
}

ZEST_CASE(NameTokens) {
    auto tokens = tokens_of("unique_ptr");
    EXPECT(llvm::is_contained(tokens, token("uni")));
    EXPECT(llvm::is_contained(tokens, token("unp")));
    EXPECT(llvm::is_contained(tokens, token("upt")));
    EXPECT(llvm::is_contained(tokens, token("ptr")));
    EXPECT(llvm::is_contained(tokens, token("u")));
    EXPECT(llvm::is_contained(tokens, token("un")));
    EXPECT(llvm::is_contained(tokens, token("up")));
    EXPECT(llvm::is_contained(tokens, token("p")));
    EXPECT(llvm::is_contained(tokens, token("pt")));
    EXPECT(!llvm::is_contained(tokens, token("n")));
    EXPECT(!llvm::is_contained(tokens, token("uq")));
    EXPECT(!llvm::is_contained(tokens, token("nqp")));
    EXPECT(llvm::is_sorted(tokens));
    EXPECT(tokens_of("").empty());
    EXPECT(tokens_of("__").empty());
    // Short tokens come from the first two heads only.
    auto three = tokens_of("foo_bar_baz");
    EXPECT(llvm::is_contained(three, token("fb")));
    EXPECT(llvm::is_contained(three, token("b")));
    EXPECT(llvm::is_contained(three, token("bb")));
    EXPECT(!llvm::is_contained(three, token("bz")));
}

ZEST_CASE(QueryTokens) {
    llvm::SmallVector<NameToken> tokens;
    query_tokens("getFoo", tokens);
    EXPECT(tokens.size() == std::size_t(4));
    EXPECT(llvm::is_contained(tokens, token("get")));
    EXPECT(llvm::is_contained(tokens, token("etf")));
    EXPECT(llvm::is_contained(tokens, token("tfo")));
    EXPECT(llvm::is_contained(tokens, token("foo")));
    query_tokens("u_p", tokens);
    EXPECT(tokens.size() == std::size_t(1));
    EXPECT(tokens.front() == token("up"));
    query_tokens("X", tokens);
    EXPECT(tokens.front() == token("x"));
    query_tokens("::", tokens);
    EXPECT(tokens.empty());

    llvm::SmallVector<TypoAlternative> alternatives;
    typo_tokens("strcp", alternatives);
    EXPECT(alternatives.empty());
    typo_tokens("strcpy", alternatives);
    EXPECT(alternatives.size() == std::size_t(6));
    // Wrong third letter: nothing survives before it, `cpy` after.
    EXPECT(alternatives[2].tokens.size() == std::size_t(1));
    EXPECT(alternatives[2].tokens.front() == token("cpy"));
    for(auto& alternative: alternatives) {
        EXPECT(!alternative.tokens.empty());
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
    EXPECT(uncovered == "");
    EXPECT(accepted > std::size_t(100));
    EXPECT(typo_accepted > std::size_t(100));
}

};  // namespace clice::testing

}  // namespace
}  // namespace clice::testing
