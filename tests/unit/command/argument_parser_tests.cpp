#include "test/test.h"
#include "command/argument_parser.h"

namespace clice::testing {

namespace {

using namespace option;

ZEST_SUITE(ArgumentParser) {

unsigned parse_first(std::vector<std::string> args) {
    for(auto& result: option::table().parse(args)) {
        if(result.has_value()) {
            return result->id;
        }
    }
    return OPT_INVALID;
}

ZEST_CASE(ParseOptionID) {
    ZASSERT(parse_first({"-g"}) == OPT_g_Flag);
    ZASSERT(parse_first({"-v"}) == OPT_v);
    ZASSERT(parse_first({"-c"}) == OPT_c);
    ZASSERT(parse_first({"-pedantic"}) == OPT_pedantic);
    ZASSERT(parse_first({"--pedantic"}) == OPT_pedantic);
    ZASSERT(parse_first({"-Wno-unused-variable"}) == OPT_W_Joined);
    ZASSERT(parse_first({"-Xclang", "-ast-dump"}) == OPT_Xclang);
    ZASSERT(parse_first({"-Wl,foo"}) == OPT_Wl_COMMA);
    ZASSERT(parse_first({"-o", "out.o"}) == OPT_o);
    ZASSERT(parse_first({"-omain.o"}) == OPT_o);
    ZASSERT(parse_first({"-I", "/usr/include"}) == OPT_I);
    ZASSERT(parse_first({"-x", "c++"}) == OPT_x);
};

ZEST_CASE(InputAndUnknown) {
    ZASSERT(parse_first({"main.cpp"}) == OPT_INPUT);
    ZASSERT(parse_first({"--clice-unknown-flag"}) == OPT_UNKNOWN);
};

ZEST_CASE(AliasAndDashDash) {
    ZASSERT(parse_first({"--include-directory=/usr/include"}) == OPT_I);
    ZASSERT(parse_first({"--language=c++"}) == OPT_x);
    ZASSERT(parse_first({"--std=c++20"}) == OPT_std_EQ);

    std::vector<std::string> args = {"-I", "/usr/include", "--", "main.cpp"};
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true};
    unsigned count = 0;
    for(auto& result: option::table().parse(args, options)) {
        if(result.has_value()) {
            ++count;
        }
    }
    ZASSERT(count == 2u);
};

ZEST_CASE(ParseError) {
    std::vector<std::string> args = {"-o"};
    bool got_error = false;
    for(auto& result: option::table().parse(args)) {
        if(!result.has_value()) {
            got_error = true;
        }
    }
    ZEXPECT(got_error);
};

ZEST_CASE(CLVisibility) {
    auto cl_vis = default_visibility("clang-cl");
    auto gcc_vis = default_visibility("clang++");

    auto parse_with_vis = [](std::vector<std::string> args, unsigned vis) -> unsigned {
        auto options = kota::option::ParseOptions{.dash_dash_parsing = true, .visibility = vis};
        for(auto& result: option::table().parse(args, options)) {
            if(result.has_value()) {
                return result->id;
            }
        }
        return OPT_INVALID;
    };

    ZASSERT(parse_with_vis({"/DFOO"}, cl_vis) == OPT_D);
    ZASSERT(parse_with_vis({"-DFOO"}, gcc_vis) == OPT_D);
    ZASSERT(parse_with_vis({"-DFOO"}, cl_vis) == OPT_D);
    /// /D carries the DXC visibility bit besides CL; a Unix-driver mask must
    /// exclude both or /Data-style paths misparse.
    ZASSERT(parse_with_vis({"/DFOO"}, gcc_vis) == OPT_INPUT);
};

ZEST_CASE(RenderRoundTrip) {
    auto roundtrip = [](std::vector<std::string> input) -> std::vector<std::string> {
        std::vector<std::string> rendered;
        for(auto& result: option::table().parse(input)) {
            if(!result.has_value())
                continue;
            auto cb = [&](std::string_view s) {
                rendered.emplace_back(s);
            };
            option::table().render(*result, cb);
        }
        return rendered;
    };

    auto r1 = roundtrip({"-I", "/usr/include"});
    ZASSERT(r1.size() == 2u);
    ZASSERT(r1[0] == "-I");
    ZASSERT(r1[1] == "/usr/include");

    auto r2 = roundtrip({"-DFOO=bar"});
    ZASSERT(r2.size() == 2u);
    ZASSERT(r2[0] == "-D");
    ZASSERT(r2[1] == "FOO=bar");

    auto r3 = roundtrip({"-Wno-unused"});
    ZASSERT(r3.size() == 1u);
    ZASSERT(r3[0] == "-Wno-unused");

    auto r4 = roundtrip({"-std=c++20"});
    ZASSERT(r4.size() == 1u);
    ZASSERT(r4[0] == "-std=c++20");
};

ZEST_CASE(PrintArgv) {
    std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
    ZASSERT(print_argv(args) == "clang++ -std=c++20 main.cpp");

    std::vector<const char*> empty = {};
    ZASSERT(print_argv(empty) == "");

    std::vector<const char*> spaced = {"clang++", "-DFOO=hello world"};
    auto result = print_argv(spaced);
    ZEXPECT(llvm::StringRef(result).contains("\""));

    std::vector<const char*> escaped = {"clang++", "-DPATH=C:\\foo"};
    auto result2 = print_argv(escaped);
    ZEXPECT(llvm::StringRef(result2).contains("\""));
};

};  // ZEST_SUITE(ArgumentParser)

}  // namespace

}  // namespace clice::testing
