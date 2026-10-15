module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :unittests.utils;

namespace clice::testing {

namespace {

std::string canon(std::vector<std::string> args) {
    return canonicalize(args);
}

ZEST_SUITE(Canonicalize) {

ZEST_CASE(StableForSameArgs) {
    std::vector<std::string> args = {"clang++", "-std=c++20", "-DFOO=1", "-I/usr/include"};
    ZASSERT(canon(args) == canon(args));
}

ZEST_CASE(CodegenFlagsIgnored) {
    // The point of the key: pure codegen flags must not change it.
    // Note -O* is deliberately NOT here: it defines
    // __OPTIMIZE__ and is kept by is_codegen_option() on purpose.
    auto base = canon({"clang++", "-std=c++20", "-DFOO", "main.cpp"});
    ZASSERT(base == canon({"clang++", "-g", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canon({"clang++", "-gdwarf-4", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canon({"clang++", "-std=c++20", "-fPIC", "-DFOO", "main.cpp"}));
    ZASSERT(base == canon({"clang++", "-std=c++20", "-DFOO", "-flto", "main.cpp"}));
    ZASSERT(base == canon({"clang++", "-std=c++20", "-ffunction-sections", "-DFOO", "main.cpp"}));
    // Driver-ignored flags: bazel stamps a per-file -frandom-seed=<output>.
    ZASSERT(base ==
            canon({"clang++", "-frandom-seed=out/main.o", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canon({"clang++", "-std=c++20", "-fgcse", "-DFOO", "main.cpp"}));
}

ZEST_CASE(SemanticFlagsChangeKey) {
    // -D/-I/-std differences must produce different keys.
    auto base = canon({"clang++", "-std=c++20", "-DFOO", "-I/a"});
    ZASSERT(base != canon({"clang++", "-std=c++20", "-DBAR", "-I/a"}));
    ZASSERT(base != canon({"clang++", "-std=c++20", "-DFOO", "-I/b"}));
    ZASSERT(base != canon({"clang++", "-std=c++17", "-DFOO", "-I/a"}));
    ZASSERT(base != canon({"clang++", "-std=c++20", "-DFOO", "-DBAR", "-I/a"}));
    ZASSERT(base != canon({"clang++", "-std=c++20", "-UFOO", "-I/a"}));
    ZASSERT(base != canon({"clang++", "-std=c++20", "-DFOO", "-isystem", "/sys", "-I/a"}));
}

ZEST_CASE(InputFileExcluded) {
    // The source file must stay out of the key so identical preambles
    // are shared across files.
    ZASSERT(canon({"clang++", "-std=c++20", "a.cpp"}) == canon({"clang++", "-std=c++20", "b.cpp"}));
    // Absolute paths too (the CDB always carries absolute source paths).
    ZASSERT(canon({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/a.cpp"}) ==
            canon({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/b.cpp"}));
    // cc1 commands carry per-file -main-file-name (injected by to_argv);
    // it labels diagnostics only and must stay out of the key.
    ZASSERT(canon({"clang-20", "-cc1", "-main-file-name", "a.cpp", "-std=c++17", "/x/a.cpp"}) ==
            canon({"clang-20", "-cc1", "-main-file-name", "b.cpp", "-std=c++17", "/x/b.cpp"}));
}

ZEST_CASE(OrderPreserved) {
    // -I/-D relative order is semantically significant; canonicalize
    // filters but never reorders.
    ZASSERT(canon({"clang++", "-I/a", "-I/b"}) != canon({"clang++", "-I/b", "-I/a"}));
    ZASSERT(canon({"clang++", "-DFOO", "-UFOO"}) != canon({"clang++", "-UFOO", "-DFOO"}));
}

ZEST_CASE(DriverIncluded) {
    // clang vs clang++ changes language defaults.
    ZASSERT(canon({"clang", "-DFOO"}) != canon({"clang++", "-DFOO"}));
}

ZEST_CASE(SpellingNormalized) {
    // Alias spellings render identically after parsing.
    ZASSERT(canon({"clang++", "-I", "/usr/include"}) == canon({"clang++", "-I/usr/include"}));
    ZASSERT(canon({"clang++", "--include-directory=/usr/include"}) ==
            canon({"clang++", "-I/usr/include"}));
}

ZEST_CASE(UnknownOptionKept) {
    // Unknown options might be semantic; dropping them could merge keys
    // that must differ.
    ZASSERT(canon({"clang++", "-DFOO"}) != canon({"clang++", "-DFOO", "-fplugin-arg-x-y"}));
}

ZEST_CASE(FrontendKeepsWarnings) {
    // Warnings affect diagnostics, which an LSP must reproduce.
    ZASSERT(canon({"clang++", "-DFOO"}) != canon({"clang++", "-Wall", "-DFOO"}));
}

};  // ZEST_SUITE(Canonicalize)

}  // namespace

}  // namespace clice::testing
