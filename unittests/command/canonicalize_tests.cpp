module;

#include "modules/prelude.h"

module clice;

import :command.argument_parser;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(Canonicalize) {

ZEST_CASE(StableForSameArgs) {
    std::vector<std::string> args = {"clang++", "-std=c++20", "-DFOO=1", "-I/usr/include"};
    ZASSERT(canonicalize(args) == canonicalize(args));
}

ZEST_CASE(CodegenFlagsIgnored) {
    // The point of the key: pure codegen flags must not change it.
    // Note -O* is deliberately NOT here: it defines
    // __OPTIMIZE__ and is kept by is_codegen_option() on purpose.
    auto base = canonicalize({"clang++", "-std=c++20", "-DFOO", "main.cpp"});
    ZASSERT(base == canonicalize({"clang++", "-g", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canonicalize({"clang++", "-gdwarf-4", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canonicalize({"clang++", "-std=c++20", "-fPIC", "-DFOO", "main.cpp"}));
    ZASSERT(base ==
            canonicalize({"clang++", "-std=c++20", "-ffunction-sections", "-DFOO", "main.cpp"}));
    // Driver-ignored flags: bazel stamps a per-file -frandom-seed=<output>.
    ZASSERT(
        base ==
        canonicalize({"clang++", "-frandom-seed=out/main.o", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canonicalize({"clang++", "-std=c++20", "-fgcse", "-DFOO", "main.cpp"}));
}

ZEST_CASE(SemanticFlagsChangeKey) {
    // -D/-I/-std differences must produce different keys.
    auto base = canonicalize({"clang++", "-std=c++20", "-DFOO", "-I/a"});
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DBAR", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-I/b"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++17", "-DFOO", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-DBAR", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-UFOO", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-isystem", "/sys", "-I/a"}));
}

ZEST_CASE(InputFileExcluded) {
    // The source file must stay out of the key so identical preambles
    // are shared across files.
    ZASSERT(canonicalize({"clang++", "-std=c++20", "a.cpp"}) ==
            canonicalize({"clang++", "-std=c++20", "b.cpp"}));
    // Absolute paths too (the CDB always carries absolute source paths).
    ZASSERT(canonicalize({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/a.cpp"}) ==
            canonicalize({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/b.cpp"}));
    // cc1 commands carry per-file -main-file-name (injected by to_argv);
    // it labels diagnostics only and must stay out of the key.
    ZASSERT(
        canonicalize({"clang-20", "-cc1", "-main-file-name", "a.cpp", "-std=c++17", "/x/a.cpp"}) ==
        canonicalize({"clang-20", "-cc1", "-main-file-name", "b.cpp", "-std=c++17", "/x/b.cpp"}));
}

ZEST_CASE(OrderPreserved) {
    // -I/-D relative order is semantically significant; canonicalize
    // filters but never reorders.
    ZASSERT(canonicalize({"clang++", "-I/a", "-I/b"}) != canonicalize({"clang++", "-I/b", "-I/a"}));
    ZASSERT(canonicalize({"clang++", "-DFOO", "-UFOO"}) !=
            canonicalize({"clang++", "-UFOO", "-DFOO"}));
}

ZEST_CASE(DriverIncluded) {
    // clang vs clang++ changes language defaults.
    ZASSERT(canonicalize({"clang", "-DFOO"}) != canonicalize({"clang++", "-DFOO"}));
}

ZEST_CASE(SpellingNormalized) {
    // Alias spellings render identically after parsing.
    ZASSERT(canonicalize({"clang++", "-I", "/usr/include"}) ==
            canonicalize({"clang++", "-I/usr/include"}));
    ZASSERT(canonicalize({"clang++", "--include-directory=/usr/include"}) ==
            canonicalize({"clang++", "-I/usr/include"}));
}

ZEST_CASE(UnknownOptionKept) {
    // Unknown options might be semantic; dropping them could merge keys
    // that must differ.
    ZASSERT(canonicalize({"clang++", "-DFOO"}) !=
            canonicalize({"clang++", "-DFOO", "-fplugin-arg-x-y"}));
}

ZEST_CASE(WarningsChangeKey) {
    // Warnings affect diagnostics, which an LSP must reproduce.
    ZASSERT(canonicalize({"clang++", "-DFOO"}) != canonicalize({"clang++", "-Wall", "-DFOO"}));
}

};  // ZEST_SUITE(Canonicalize)

}  // namespace

}  // namespace clice::testing
