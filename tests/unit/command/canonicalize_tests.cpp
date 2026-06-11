#include <string>
#include <vector>

#include "test/test.h"
#include "command/argument_parser.h"

namespace clice::testing {

namespace {

std::string canon(std::vector<std::string> args, ArgsProfile profile = ArgsProfile::Frontend) {
    return canonicalize(args, profile);
}

TEST_SUITE(Canonicalize) {

TEST_CASE(StableForSameArgs) {
    std::vector<std::string> args = {"clang++", "-std=c++20", "-DFOO=1", "-I/usr/include"};
    ASSERT_EQ(canon(args), canon(args));
}

TEST_CASE(CodegenFlagsIgnored) {
    // The point of the Frontend profile: pure codegen flags must not
    // change the key.  Note -O* is deliberately NOT here: it defines
    // __OPTIMIZE__ and is kept by is_codegen_option() on purpose.
    auto base = canon({"clang++", "-std=c++20", "-DFOO", "main.cpp"});
    ASSERT_EQ(base, canon({"clang++", "-g", "-std=c++20", "-DFOO", "main.cpp"}));
    ASSERT_EQ(base, canon({"clang++", "-gdwarf-4", "-std=c++20", "-DFOO", "main.cpp"}));
    ASSERT_EQ(base, canon({"clang++", "-std=c++20", "-fPIC", "-DFOO", "main.cpp"}));
    ASSERT_EQ(base, canon({"clang++", "-std=c++20", "-DFOO", "-flto", "main.cpp"}));
    ASSERT_EQ(base, canon({"clang++", "-std=c++20", "-ffunction-sections", "-DFOO", "main.cpp"}));
}

TEST_CASE(SemanticFlagsChangeKey) {
    // -D/-I/-std differences must produce different keys.
    auto base = canon({"clang++", "-std=c++20", "-DFOO", "-I/a"});
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-DBAR", "-I/a"}));
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-DFOO", "-I/b"}));
    ASSERT_NE(base, canon({"clang++", "-std=c++17", "-DFOO", "-I/a"}));
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-DFOO", "-DBAR", "-I/a"}));
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-UFOO", "-I/a"}));
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-DFOO", "-isystem", "/sys", "-I/a"}));
}

TEST_CASE(InputFileExcluded) {
    // The source file must stay out of the key so identical preambles
    // are shared across files.
    ASSERT_EQ(canon({"clang++", "-std=c++20", "a.cpp"}), canon({"clang++", "-std=c++20", "b.cpp"}));
    // Absolute paths too (the CDB always carries absolute source paths).
    ASSERT_EQ(canon({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/a.cpp"}),
              canon({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/b.cpp"}));
    // cc1 commands carry per-file -main-file-name (injected by to_argv);
    // it labels diagnostics only and must stay out of the key.
    ASSERT_EQ(canon({"clang-20", "-cc1", "-main-file-name", "a.cpp", "-std=c++17", "/x/a.cpp"}),
              canon({"clang-20", "-cc1", "-main-file-name", "b.cpp", "-std=c++17", "/x/b.cpp"}));
}

TEST_CASE(OrderPreserved) {
    // -I/-D relative order is semantically significant; canonicalize
    // filters but never reorders.
    ASSERT_NE(canon({"clang++", "-I/a", "-I/b"}), canon({"clang++", "-I/b", "-I/a"}));
    ASSERT_NE(canon({"clang++", "-DFOO", "-UFOO"}), canon({"clang++", "-UFOO", "-DFOO"}));
}

TEST_CASE(DriverIncluded) {
    // clang vs clang++ changes language defaults.
    ASSERT_NE(canon({"clang", "-DFOO"}), canon({"clang++", "-DFOO"}));
}

TEST_CASE(SpellingNormalized) {
    // Alias spellings render identically after parsing.
    ASSERT_EQ(canon({"clang++", "-I", "/usr/include"}), canon({"clang++", "-I/usr/include"}));
    ASSERT_EQ(canon({"clang++", "--include-directory=/usr/include"}),
              canon({"clang++", "-I/usr/include"}));
}

TEST_CASE(UnknownOptionKept) {
    // Unknown options might be semantic; dropping them could merge keys
    // that must differ.
    ASSERT_NE(canon({"clang++", "-DFOO"}), canon({"clang++", "-DFOO", "-fplugin-arg-x-y"}));
}

TEST_CASE(PreprocessingDropsWarnings) {
    auto base = canon({"clang++", "-std=c++20", "-DFOO"}, ArgsProfile::Preprocessing);
    ASSERT_EQ(base, canon({"clang++", "-std=c++20", "-Wall", "-DFOO"}, ArgsProfile::Preprocessing));
    ASSERT_EQ(
        base,
        canon({"clang++", "-std=c++20", "-DFOO", "-Werror", "-w"}, ArgsProfile::Preprocessing));
    ASSERT_EQ(base,
              canon({"clang++", "-pedantic", "-std=c++20", "-DFOO"}, ArgsProfile::Preprocessing));
    // But preprocessing-relevant options still matter.
    ASSERT_NE(base, canon({"clang++", "-std=c++20", "-DBAR"}, ArgsProfile::Preprocessing));
    ASSERT_NE(base, canon({"clang++", "-std=c++17", "-DFOO"}, ArgsProfile::Preprocessing));
}

TEST_CASE(FrontendKeepsWarnings) {
    // Warnings affect diagnostics, which an LSP must reproduce.
    ASSERT_NE(canon({"clang++", "-DFOO"}), canon({"clang++", "-Wall", "-DFOO"}));
}

TEST_CASE(FullKeepsCodegen) {
    ASSERT_NE(canon({"clang++", "-DFOO"}, ArgsProfile::Full),
              canon({"clang++", "-O2", "-DFOO"}, ArgsProfile::Full));
}

};  // TEST_SUITE(Canonicalize)

}  // namespace

}  // namespace clice::testing
