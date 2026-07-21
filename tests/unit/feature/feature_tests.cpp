#include "test/test.h"
#include "feature/feature.h"

namespace clice::testing {

namespace {

TEST_SUITE(ToUri) {

TEST_CASE(WindowsDrivePath) {
    // A drive letter must not be mistaken for a URI scheme.
    ASSERT_EQ(feature::to_uri("F:/C++/cmake/clice/main.cpp"),
              "file:///F:/C++/cmake/clice/main.cpp");
}

TEST_CASE(WindowsBackslashPath) {
    ASSERT_EQ(feature::to_uri(R"(F:\C++\cmake\clice\main.cpp)"),
              "file:///F:/C++/cmake/clice/main.cpp");
}

TEST_CASE(PosixPath) {
    ASSERT_EQ(feature::to_uri("/home/user/main.cpp"), "file:///home/user/main.cpp");
}

TEST_CASE(FormedUri) {
    ASSERT_EQ(feature::to_uri("file:///home/user/main.cpp"), "file:///home/user/main.cpp");
}

TEST_CASE(RelativePath) {
    // Neither an absolute path nor a URI: returned verbatim.
    ASSERT_EQ(feature::to_uri("include/test.h"), "include/test.h");
}

TEST_CASE(PathWithSpaces) {
    ASSERT_EQ(feature::to_uri("/home/user/my file.cpp"), "file:///home/user/my%20file.cpp");
}

TEST_CASE(UncPath) {
    ASSERT_EQ(feature::to_uri("//server/share/main.cpp"), "file://server/share/main.cpp");
}

};  // TEST_SUITE(ToUri)

}  // namespace

}  // namespace clice::testing
