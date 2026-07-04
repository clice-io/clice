#include "test/test.h"
#include "syntax/preamble_synthesis.h"

#include "llvm/ADT/StringMap.h"

namespace clice::testing {
namespace {

/// Build a resolver from a fixed raw-name -> absolute-path mapping.
auto map_resolver(const llvm::StringMap<std::string>& mapping) {
    return [&mapping](llvm::StringRef name,
                      [[maybe_unused]] bool is_angled,
                      [[maybe_unused]] bool is_include_next,
                      [[maybe_unused]] llvm::StringRef includer_dir) -> std::optional<std::string> {
        auto it = mapping.find(name);
        if(it == mapping.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

TEST_SUITE(PreambleSynthesis) {

TEST_CASE(BasicChain) {
    llvm::StringMap<std::string> mapping = {
        {"vector",  "/sys/vector"  },
        {"utils.h", "/proj/utils.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include <vector>
#define DEBUG 1
#include "utils.h"
int main() {}
)"};

    auto result = synthesize_preamble({entry}, "/proj/utils.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include <vector>
#define DEBUG 1
)");
}

TEST_CASE(MultiLevelChain) {
    llvm::StringMap<std::string> mapping = {
        {"utils.h", "/proj/utils.h"},
        {"string",  "/sys/string"  },
        {"math.h",  "/proj/math.h" },
    };

    ChainEntry main_entry{"/proj/main.cpp", R"(#include "utils.h"
int main() {}
)"};
    ChainEntry utils_entry{"/proj/utils.h", R"(#pragma once
#include <string>
#include "math.h"
void util_func();
)"};

    auto result =
        synthesize_preamble({main_entry, utils_entry}, "/proj/math.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#line 1 "/proj/utils.h"
#pragma once
#include <string>
)");
}

TEST_CASE(SameBasenameHeaders) {
    // Two headers with the same filename in different directories: the
    // resolver must disambiguate, a filename match would pick the wrong one.
    llvm::StringMap<std::string> mapping = {
        {"a/config.h", "/proj/a/config.h"},
        {"b/config.h", "/proj/b/config.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include "a/config.h"
#include "b/config.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/b/config.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include "/proj/a/config.h"
)");
}

TEST_CASE(QuotedIncludeAbsolutized) {
    // The preamble file lives in the cache directory, so quoted includes
    // must be rewritten to absolute paths; angled includes stay untouched.
    llvm::StringMap<std::string> mapping = {
        {"vector",  "/sys/vector"  },
        {"types.h", "/proj/types.h"},
        {"utils.h", "/proj/utils.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include <vector>
#include "types.h"
#include "utils.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/utils.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include <vector>
#include "/proj/types.h"
)");
}

TEST_CASE(FilenameFallback) {
    // Resolution fails entirely — an unambiguous filename match still works.
    llvm::StringMap<std::string> empty;

    ChainEntry entry{"/proj/main.cpp", R"(#include <vector>
#include "utils.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/utils.h", map_resolver(empty));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include <vector>
)");
}

TEST_CASE(AmbiguousFallbackFails) {
    // Resolution fails and two candidates share the target's filename:
    // refuse to guess.
    llvm::StringMap<std::string> empty;

    ChainEntry entry{"/proj/main.cpp", R"(#include "a/config.h"
#include "b/config.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/b/config.h", map_resolver(empty));
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(NoMatchFails) {
    llvm::StringMap<std::string> mapping = {
        {"vector", "/sys/vector"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include <vector>
)"};

    auto result = synthesize_preamble({entry}, "/proj/utils.h", map_resolver(mapping));
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(CommentedIncludeIgnored) {
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(/*
#include "target.h"
*/
#include "target.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
/*
#include "target.h"
*/
)");
}

TEST_CASE(LineMarkerEscaping) {
    llvm::StringMap<std::string> mapping = {
        {"utils.h", R"(C:\proj\utils.h)"},
    };

    ChainEntry entry{R"(C:\proj\main.cpp)", R"(#include "utils.h"
)"};

    auto result = synthesize_preamble({entry}, R"(C:\proj\utils.h)", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "C:\\proj\\main.cpp"
)");
}

TEST_CASE(ConditionalShadowSkipped) {
    // An include of the target inside an #if block must not shadow the
    // real, unconditional one — the cut lands at the latter.
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#if 0
#include "target.h"
#endif
#define X 1
#include "target.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    // The shadowed occurrence of the target itself is blanked (line kept):
    // at compile time the target's path is remapped to the open buffer.
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#if 0

#endif
#define X 1
)");
}

TEST_CASE(GuardChainBalanced) {
    // Cutting inside a classic include guard must close the open #ifndef,
    // or clang reports an unterminated conditional in the preamble.
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/a.h", R"(#ifndef A_H
#define A_H
struct A {};
#include "target.h"
#endif
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/a.h"
#ifndef A_H
#define A_H
struct A {};
#endif
)");
}

TEST_CASE(OnlyConditionalMatch) {
    // Platform-conditional include: the only match is conditional, so the
    // cut lands inside the block and the #ifdef is balanced.
    llvm::StringMap<std::string> mapping = {
        {"impl.h", "/proj/impl.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#ifdef _WIN32
#include "impl.h"
#endif
)"};

    auto result = synthesize_preamble({entry}, "/proj/impl.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#ifdef _WIN32
#endif
)");
}

TEST_CASE(DuplicateSpellingFallback) {
    // Resolution fails and two directives share the same raw spelling:
    // they bring in the same file, so prefer the unconditional one instead
    // of treating this as ambiguous.
    llvm::StringMap<std::string> empty;

    ChainEntry entry{"/proj/main.cpp", R"(#ifdef FAST
#include "impl.h"
#endif
#include "impl.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/impl.h", map_resolver(empty));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#ifdef FAST
#include "impl.h"
#endif
)");
}

TEST_CASE(DuplicateIncludeFirstCut) {
    // Two identical unconditional includes of the target: cut at the first.
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include "target.h"
#define Y 1
#include "target.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
)");
}

TEST_CASE(MacroIncludeIgnored) {
    // #include MACRO carries no header-name token; scan() skips it, so it
    // is kept verbatim and never considered for matching.
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include CONFIG_H
#include "target.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include CONFIG_H
)");
}

TEST_CASE(IncludeNextKeptVerbatim) {
    // #include_next search-resume semantics cannot survive relocation into
    // the cache directory; it is deliberately not rewritten.
    llvm::StringMap<std::string> mapping = {
        {"impl.h",   "/x/impl.h"     },
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include_next "impl.h"
#include "target.h"
)"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, R"(#line 1 "/proj/main.cpp"
#include_next "impl.h"
)");
}

TEST_CASE(OccurrenceSelectsMatch) {
    // Explicit occurrence indexes the candidate list of the direct
    // includer, overriding the prefer-unconditional default.
    llvm::StringMap<std::string> mapping = {
        {"list.def", "/proj/list.def"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#define X(name) int name;
#include "list.def"
#undef X
#define X(name) void get_##name();
#include "list.def"
)"};

    auto second =
        synthesize_preamble({entry}, "/proj/list.def", map_resolver(mapping), std::uint32_t(1));
    ASSERT_TRUE(second.has_value());
    // The other occurrence of the target is blanked (line kept).
    EXPECT_EQ(*second, R"(#line 1 "/proj/main.cpp"
#define X(name) int name;

#undef X
#define X(name) void get_##name();
)");

    auto first =
        synthesize_preamble({entry}, "/proj/list.def", map_resolver(mapping), std::uint32_t(0));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, R"(#line 1 "/proj/main.cpp"
#define X(name) int name;
)");
}

TEST_CASE(OccurrenceOutOfRange) {
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(#include "target.h"
)"};

    auto result =
        synthesize_preamble({entry}, "/proj/target.h", map_resolver(mapping), std::uint32_t(1));
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(CountOccurrences) {
    llvm::StringMap<std::string> empty;

    auto count = count_include_occurrences(R"(#include "list.def"
#undef X
#include "list.def"
)",
                                           "/proj/main.cpp",
                                           "/proj/list.def",
                                           map_resolver(empty));
    EXPECT_EQ(count, 2u);
}

TEST_CASE(CrlfLineEndings) {
    llvm::StringMap<std::string> empty;

    // CR cannot appear in a raw literal cleanly; escaped string is clearer.
    ChainEntry entry{"/proj/main.cpp", "#include \"a.h\"\r\n#include \"target.h\"\r\n"};

    auto result = synthesize_preamble({entry}, "/proj/target.h", map_resolver(empty));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "#line 1 \"/proj/main.cpp\"\n#include \"a.h\"\r\n");
}

TEST_CASE(EmptyChain) {
    llvm::StringMap<std::string> empty;

    auto result =
        synthesize_preamble(llvm::ArrayRef<ChainEntry>(), "/proj/x.h", map_resolver(empty));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
}

TEST_CASE(SuffixClosesBraces) {
    // Function-body X-macro: the suffix restores the closing brace and
    // trailing directives after the include position.
    llvm::StringMap<std::string> mapping = {
        {"errors.def", "/proj/errors.def"},
    };

    ChainEntry entry{"/proj/main.cpp", R"(void register_all() {
#define X(name) handle(name);
#include "errors.def"
#undef X
}
)"};

    auto result = synthesize_context({entry}, "/proj/errors.def", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->prefix, R"(#line 1 "/proj/main.cpp"
void register_all() {
#define X(name) handle(name);
)");
    EXPECT_EQ(result->suffix, R"(#line 4 "/proj/main.cpp"
#undef X
}
)");
}

TEST_CASE(SuffixReopensGuard) {
    // The prefix closed the include guard early with a balancing #endif;
    // the suffix reopens it with `#if 1` so its own #endif stays matched.
    llvm::StringMap<std::string> mapping = {
        {"target.h", "/proj/target.h"},
    };

    ChainEntry entry{"/proj/a.h", R"(#ifndef A_H
#define A_H
#include "target.h"
void tail();
#endif
)"};

    auto result = synthesize_context({entry}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->prefix, R"(#line 1 "/proj/a.h"
#ifndef A_H
#define A_H
#endif
)");
    EXPECT_EQ(result->suffix, R"(#if 1
#line 4 "/proj/a.h"
void tail();
#endif
)");
}

TEST_CASE(SuffixMirrorsChain) {
    // Multi-level: the suffix is assembled innermost-first, mirroring the
    // prefix's host-first order.
    llvm::StringMap<std::string> mapping = {
        {"mid.h",    "/proj/mid.h"   },
        {"target.h", "/proj/target.h"},
    };

    ChainEntry host{"/proj/main.cpp", R"(#include "mid.h"
int main() {}
)"};
    ChainEntry mid{"/proj/mid.h", R"(#include "target.h"
void mid_tail();
)"};

    auto result = synthesize_context({host, mid}, "/proj/target.h", map_resolver(mapping));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->suffix, R"(#line 2 "/proj/mid.h"
void mid_tail();
#line 2 "/proj/main.cpp"
int main() {}
)");
}

};  // TEST_SUITE(PreambleSynthesis)

}  // namespace
}  // namespace clice::testing
