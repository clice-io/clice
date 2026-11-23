module clice;

import :feature.protocol;
import :unittests.utils;
import :vfs.file_table;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(ToUri) {

#ifdef _WIN32
ZEST_CASE(WindowsDrivePath) {
    // A drive letter must not be mistaken for a URI scheme, and it keeps
    // its case: a client keys documents by its own spelling. These inputs
    // are ordinary (odd) filenames elsewhere.
    ZASSERT(feature::to_uri("F:/C++/cmake/clice/main.cpp") ==
            "file:///F:/C++/cmake/clice/main.cpp");
    ZASSERT(feature::to_uri("f:/C++/cmake/clice/main.cpp") ==
            "file:///f:/C++/cmake/clice/main.cpp");
}

ZEST_CASE(WindowsBackslashPath) {
    ZASSERT(feature::to_uri(R"(F:\C++\cmake\clice\main.cpp)") ==
            "file:///F:/C++/cmake/clice/main.cpp");
}

ZEST_CASE(WindowsClientSpellingKept) {
    // A URI the client sends goes back unchanged through the path it
    // writes; the file's spelling lowers the drive.
    llvm::StringRef uri = "file:///C:/Users/a/main.cpp";
    auto written = feature::written_path(uri);
    ZASSERT(written == "C:/Users/a/main.cpp");
    ZASSERT(feature::to_uri(*written) == uri);
    ZASSERT(feature::to_path(uri)->str() == "c:/Users/a/main.cpp");
}
#endif

ZEST_CASE(PlusStaysLiteral) {
    // kotatsu keeps '+' unencoded; clients percent-encode it. canonicalUri on
    // the harness side reconciles — but the encoded form itself is pinned
    // here so an encoding-table change cannot slip by silently.
    ZASSERT(feature::to_uri("/home/user/a+b.h") == "file:///home/user/a+b.h");
}

ZEST_CASE(RoundTripIdentity) {
    // Ingest of an emitted URI must intern to the same ID as the original
    // canonical path — including the client-style encoded-colon spelling.
    FileTable pool;
    constexpr Fid bad{0xFFFFFFFF};
    auto ingest = [&](llvm::StringRef uri) -> Fid {
        auto parsed = lsp::URI::from_string(uri);
        std::string path;
        if(!parsed || parsed->to_path(path)) {
            return bad;
        }
        return pool.intern(Spelling::absolute(path));
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
