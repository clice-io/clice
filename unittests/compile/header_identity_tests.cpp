module clice;

import :unittests.utils;
import :unittests.utils.tester;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(HeaderIdentity, Tester) {

/// The identity `file` gets in a background index's compile of `focus`.
std::uint64_t identity(Tester& tester,
                       llvm::StringRef focus,
                       llvm::StringRef file,
                       llvm::StringRef standard = "-std=c++20") {
    tester.src_path = focus.str();
    tester.kind = CompilationKind::Whole;
    tester.prepare(standard);
    tester.params.header_identities = true;
    if(!tester.try_compile()) {
        return ~0ull;
    }
    auto fid = tester.unit->file_id(TestVFS::path(file));
    return tester.unit->header_identities().lookup(fid);
}

ZEST_CASE(SameTextSameIdentity) {
    add_file("box.h", "#pragma once\nstruct Box { int value; };\n");
    add_file("a.cpp", "#include \"box.h\"\nint a;\n");
    add_file("b.cpp", "int b;\n#include \"box.h\"\n");
    auto first = identity(*this, "a.cpp", "box.h");
    ZEXPECT(first != 0);
    ZEXPECT(identity(*this, "b.cpp", "box.h") == first);
};

ZEST_CASE(MacrosAndBranches) {
    add_file("config.h", R"cpp(
#pragma once
#ifdef WIDE
using size = long;
#else
using size = int;
#endif
inline size measure() { return LIMIT; }
)cpp");
    add_file("narrow.cpp", "#define LIMIT 1\n#include \"config.h\"\n");
    add_file("wide.cpp", "#define WIDE\n#define LIMIT 1\n#include \"config.h\"\n");
    add_file("larger.cpp", "#define LIMIT 2\n#include \"config.h\"\n");
    auto narrow = identity(*this, "narrow.cpp", "config.h");
    ZEXPECT(identity(*this, "wide.cpp", "config.h") != narrow);
    // Another definition of the macro the header expands: written at another
    // place.
    ZEXPECT(identity(*this, "larger.cpp", "config.h") != narrow);
};

ZEST_CASE(DeclarationContext) {
    add_file("part.h", "struct Part {};\n");
    add_file("top.cpp", "#include \"part.h\"\n");
    add_file("nested.cpp", "namespace inner {\n#include \"part.h\"\n}\n");
    add_file("after.cpp", "namespace inner {}\n#include \"part.h\"\n");
    auto top = identity(*this, "top.cpp", "part.h");
    ZEXPECT(identity(*this, "nested.cpp", "part.h") != top);
    // Entered right after a namespace closes: the parser read the include
    // ahead of leaving the namespace.
    ZEXPECT(identity(*this, "after.cpp", "part.h") == top);
};

ZEST_CASE(LanguageStandard) {
    add_file("plain.h", "struct Plain {};\n");
    add_file("main.cpp", "#include \"plain.h\"\n");
    ZEXPECT(identity(*this, "main.cpp", "plain.h", "-std=c++17") !=
            identity(*this, "main.cpp", "plain.h", "-std=c++20"));
};

ZEST_CASE(CounterIsTheUnitsAlone) {
    add_file("unique.h", "#pragma once\nconstexpr int unique = __COUNTER__;\n");
    add_file("main.cpp", "#include \"unique.h\"\n");
    ZEXPECT(identity(*this, "main.cpp", "unique.h") == 0U);
};

};  // ZEST_SUITE(HeaderIdentity)

}  // namespace

}  // namespace clice::testing
