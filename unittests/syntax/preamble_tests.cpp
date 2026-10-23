module;

#include "support/prelude.h"

module clice;

import :syntax.preamble;
import :unittests.utils;

namespace clice::testing {
namespace {

ZEST_SUITE(PreambleBound) {

ZEST_CASE(Empty) {
    ZEXPECT(preamble_bound("") == 0u);
}

ZEST_CASE(NoDirectives) {
    ZEXPECT(preamble_bound("int x = 1;") == 0u);
}

ZEST_CASE(SingleInclude) {
    llvm::StringRef src = R"(
#include <vector>
int x;
)";
    auto bound = preamble_bound(src);
    ZEXPECT(bound > 0u);
    ZEXPECT(bound <= src.find("int"));
}

ZEST_CASE(MultipleDirectives) {
    llvm::StringRef src = R"(
#include <vector>
#include <string>
#define FOO 1
int x;
)";
    auto bound = preamble_bound(src);
    ZEXPECT(bound > src.find("#define"));
}

ZEST_CASE(GlobalModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <vector>
export module foo;
)";
    auto bound = preamble_bound(src);
    ZEXPECT(bound > 0u);
    ZEXPECT(bound < src.size());
}

ZEST_CASE(BoundAfterLastDirective) {
    llvm::StringRef src = R"(
#include <a>
#include <b>
int x;
)";
    ZEXPECT(preamble_bound(src) == src.find("<b>") + 3);
}

ZEST_CASE(BoundWithModuleFragment) {
    llvm::StringRef src = R"(
module;
#include <a>
#include <b>
export module foo;
)";
    ZEXPECT(preamble_bound(src) == src.find("<b>") + 3);

    llvm::StringRef marker_only = R"(module;
export module foo;
)";
    ZEXPECT(preamble_bound(marker_only) == marker_only.find(';') + 1);
}

ZEST_CASE(StopsAtCode) {
    llvm::StringRef src = R"(
#include <a>
int x;
#include <b>
)";
    ZEXPECT(preamble_bound(src) == src.find("<a>") + 3);
}

ZEST_CASE(ConditionalDirectives) {
    llvm::StringRef src = R"(
#ifndef GUARD
#define GUARD
#include <a>
#endif
int x;
)";
    auto bound = preamble_bound(src);
    ZEXPECT(bound > src.find("#endif"));
}

};  // ZEST_SUITE(PreambleBound)

ZEST_SUITE(FinishedPreambleBound) {

ZEST_CASE(CompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo.h\"\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(CompleteAngledInclude) {
    llvm::StringRef content = "#include <vector>\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(IncompleteQuotedInclude) {
    llvm::StringRef content = "#include \"foo\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncompleteAngledInclude) {
    llvm::StringRef content = "#include <sys/\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncludeWithNoPath) {
    llvm::StringRef content = "#include \nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(IncompleteEmbed) {
    llvm::StringRef content = "#embed <data\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == 0u);
}

ZEST_CASE(CompleteEmbed) {
    llvm::StringRef content = "#embed \"data.bin\" limit(4)\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(IncludeMacroUsage) {
    llvm::StringRef content = "#include FOO\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MultipleIncludesAllComplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo.h\"\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MultipleIncludesLastIncomplete) {
    llvm::StringRef content = "#include <vector>\n#include \"foo\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == content.find('\n'));
}

ZEST_CASE(IncompleteIncludeMidBlock) {
    // Typed in the middle of the block: the finished includes before it
    // stay in the preamble, the ones after it go with the body.
    llvm::StringRef content = "#include <a>\n#include <vec\n#include \"c.h\"\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == content.find('\n'));
}

ZEST_CASE(SplicedIncompleteInclude) {
    // The unterminated filename token's spelling begins with the splice.
    llvm::StringRef content = "#include \\\n<foo\nint x;";
    ZEXPECT(finished_preamble_bound(content, 16) == 0u);
}

ZEST_CASE(SplicedCompleteInclude) {
    llvm::StringRef content = "#include \\\n<foo.h>\nint x;";
    ZEXPECT(finished_preamble_bound(content, 19) == 19);
}

ZEST_CASE(AngledHashImport) {
    llvm::StringRef content = "#import <foo.h>\nint x;";
    ZEXPECT(finished_preamble_bound(content, 16) == 16);
}

ZEST_CASE(EmptyPreamble) {
    llvm::StringRef content = "int x;";
    ZEXPECT(finished_preamble_bound(content, 0) == 0);
}

ZEST_CASE(PreprocessorDirectivesIgnored) {
    llvm::StringRef content = "#ifdef FOO\n#define BAR 1\n#endif\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

ZEST_CASE(MixedIncludeAndImportAllComplete) {
    llvm::StringRef content = "#include <vector>\nimport std;\nint x;";
    auto bound = preamble_bound(content);
    ZEXPECT(finished_preamble_bound(content, bound) == bound);
}

};  // ZEST_SUITE(FinishedPreambleBound)

ZEST_SUITE(PreambleUsesMacros) {

ZEST_CASE(Includes) {
    llvm::StringRef content = "#include <a>\n#include_next \"b.h\"\n#pragma message(\"m\")\nint x;";
    ZEXPECT(!preamble_uses_macros(content, preamble_bound(content)));
}

ZEST_CASE(Definition) {
    llvm::StringRef content = "#include <a>\n#define A 1\nint x;";
    ZEXPECT(preamble_uses_macros(content, preamble_bound(content)));
}

ZEST_CASE(Condition) {
    llvm::StringRef content = "#ifdef A\n#include <a>\n#endif\nint x;";
    ZEXPECT(preamble_uses_macros(content, preamble_bound(content)));
}

ZEST_CASE(DefinitionPastBound) {
    llvm::StringRef content = "#include <a>\nint x;\n#define A 1\n";
    ZEXPECT(!preamble_uses_macros(content, preamble_bound(content)));
}

};  // ZEST_SUITE(PreambleUsesMacros)

ZEST_SUITE(HasPragmaOnce) {

ZEST_CASE(InPreamble) {
    llvm::StringRef content = "#pragma once\n#include <a>\nint x;\n#include <b>\n";
    ZEXPECT(has_pragma_once(content, preamble_bound(content)));
}

ZEST_CASE(PastBound) {
    llvm::StringRef content = "#include <a>\nint x;\n#pragma once\n";
    ZEXPECT(!has_pragma_once(content, preamble_bound(content)));
    ZEXPECT(has_pragma_once(content, content.size()));
}

ZEST_CASE(OtherPragmas) {
    llvm::StringRef content = "#pragma message(\"once\")\n#pragma pack(1)\nint x;";
    ZEXPECT(!has_pragma_once(content, content.size()));
}

};  // ZEST_SUITE(HasPragmaOnce)

}  // namespace
}  // namespace clice::testing
