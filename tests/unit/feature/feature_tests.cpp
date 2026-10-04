#include "test/test.h"
#include "feature/feature.h"
#include "vfs/file_table.h"

namespace clice::testing {

namespace {

ZEST_SUITE(ToUri) {

#ifdef _WIN32
ZEST_CASE(WindowsDrivePath) {  // A drive letter must not be mistaken for a URI scheme, and it is
    // emitted lowercase — the form LSP clients key documents by. The
    // rewrite only applies on Windows; these inputs are ordinary (odd)
    // filenames elsewhere.
    ZASSERT(feature::to_uri("F:/C++/cmake/clice/main.cpp") ==
            "file:///f:/C++/cmake/clice/main.cpp");
}

ZEST_CASE(WindowsBackslashPath) {
    ZASSERT(feature::to_uri(R"(F:\C++\cmake\clice\main.cpp)") ==
            "file:///f:/C++/cmake/clice/main.cpp");
}
#endif

ZEST_CASE(PlusStaysLiteral) {
    // kota keeps '+' unencoded; clients percent-encode it. canonicalUri on
    // the harness side reconciles — but the wire form itself is pinned
    // here so an encoding-table change cannot slip by silently.
    ZASSERT(feature::to_uri("/home/user/a+b.h") == "file:///home/user/a+b.h");
}

ZEST_CASE(RoundTripIdentity) {
    // Ingest of an emitted URI must intern to the same ID as the original
    // canonical path — including the client-style encoded-colon spelling.
    FileTable pool;
    constexpr Fid bad{0xFFFFFFFF};
    auto ingest = [&](llvm::StringRef uri) -> Fid {
        auto parsed = kota::ipc::lsp::URI::parse(std::string_view(uri.data(), uri.size()));
        if(!parsed.has_value()) {
            return bad;
        }
        auto path = parsed->file_path();
        if(!path.has_value()) {
            return bad;
        }
        return pool.intern(Spelling::absolute(*path));
    };
    ZASSERT(ingest(feature::to_uri("/proj/a.cpp")) ==
            pool.intern(Spelling::absolute("/proj/a.cpp")));
#ifdef _WIN32
    ZASSERT(ingest(feature::to_uri(R"(C:\proj\a.cpp)")) ==
            pool.intern(Spelling::absolute("c:/proj/a.cpp")));
    ZASSERT(ingest("file:///c%3A/proj/a.cpp") == pool.intern(Spelling::absolute("c:/proj/a.cpp")));
#endif
}

ZEST_CASE(PosixPath) {
    ZASSERT(feature::to_uri("/home/user/main.cpp") == "file:///home/user/main.cpp");
}

ZEST_CASE(PathWithSpaces) {
    ZASSERT(feature::to_uri("/home/user/my file.cpp") == "file:///home/user/my%20file.cpp");
}

ZEST_CASE(UncPath) {
    ZASSERT(feature::to_uri("//server/share/main.cpp") == "file://server/share/main.cpp");
}

};  // ZEST_SUITE(ToUri)

}  // namespace

}  // namespace clice::testing
