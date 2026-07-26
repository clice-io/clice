#include "test/test.h"
#include "feature/feature.h"
#include "support/path_pool.h"

namespace clice::testing {

namespace {

TEST_SUITE(ToUri) {

#ifdef _WIN32
TEST_CASE(WindowsDrivePath) {
    // A drive letter must not be mistaken for a URI scheme, and it is
    // emitted lowercase — the form LSP clients key documents by. The
    // rewrite only applies on Windows; these inputs are ordinary (odd)
    // filenames elsewhere.
    ASSERT_EQ(feature::to_uri("F:/C++/cmake/clice/main.cpp"),
              "file:///f:/C++/cmake/clice/main.cpp");
}

TEST_CASE(WindowsBackslashPath) {
    ASSERT_EQ(feature::to_uri(R"(F:\C++\cmake\clice\main.cpp)"),
              "file:///f:/C++/cmake/clice/main.cpp");
}

TEST_CASE(FormedUriDriveLowered) {
    // The already-a-URI branch canonicalizes too: an uppercase drive must
    // not leak through no matter which shape a caller hands in.
    ASSERT_EQ(feature::to_uri("file:///C:/x.cpp"), "file:///c:/x.cpp");
}
#endif

TEST_CASE(PlusStaysLiteral) {
    // kota keeps '+' unencoded; clients percent-encode it. canonicalUri on
    // the harness side reconciles — but the wire form itself is pinned
    // here so an encoding-table change cannot slip by silently.
    ASSERT_EQ(feature::to_uri("/home/user/a+b.h"), "file:///home/user/a+b.h");
}

TEST_CASE(RoundTripIdentity) {
    // Ingest of an emitted URI must intern to the same ID as the original
    // canonical path — including the client-style encoded-colon spelling.
    PathPool pool;
    constexpr std::uint32_t bad = 0xFFFFFFFF;
    auto ingest = [&](llvm::StringRef uri) -> std::uint32_t {
        auto parsed = kota::ipc::lsp::URI::parse(std::string_view(uri.data(), uri.size()));
        if(!parsed.has_value()) {
            return bad;
        }
        auto path = parsed->file_path();
        if(!path.has_value()) {
            return bad;
        }
        return pool.intern(*path);
    };
    ASSERT_EQ(ingest(feature::to_uri("/proj/a.cpp")), pool.intern("/proj/a.cpp"));
#ifdef _WIN32
    ASSERT_EQ(ingest(feature::to_uri(R"(C:\proj\a.cpp)")), pool.intern("c:/proj/a.cpp"));
    ASSERT_EQ(ingest("file:///c%3A/proj/a.cpp"), pool.intern("c:/proj/a.cpp"));
#endif
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
