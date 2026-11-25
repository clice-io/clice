module clice;

import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(Path) {

/// A path taken as spelled already is what spelling it would give.
ZEST_CASE(SpelledAlready) {
    auto respelled = [](llvm::StringRef text) {
        std::string out;
        path::resolve(out, text, "", path::DotDot::Keep);
        path::canonicalize(out);
        return out;
    };
    auto root = Spelling::cwd().str().substr(0, path::native_style == path::Style::Windows ? 3 : 1);
    for(llvm::StringRef rest: {"",
                               "a",
                               "a/b.h",
                               "a/../b",
                               "..",
                               "a/..",
                               "a/.hidden",
                               "a/b./c",
                               "a/...",
                               "Engine/Source/Runtime/Core/Public"}) {
        auto text = root + rest.str();
        ZEXPECT(path::spelled_already(text));
        ZEXPECT(respelled(text) == text);
    }
    for(llvm::StringRef rest: {"a/", "a//b", "a/./b", "./a", ".", "a/.", "/a"}) {
        auto text = root + rest.str();
        ZEXPECT(!path::spelled_already(text));
        ZEXPECT(Spelling::absolute(text).str() == respelled(text));
    }
    ZEXPECT(!path::spelled_already("a/b"));
    ZEXPECT(!path::spelled_already(""));
};

};  // ZEST_SUITE(Path)

}  // namespace

}  // namespace clice::testing
