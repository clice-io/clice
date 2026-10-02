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
        ASSERT(parse_first({"-g"}) == OPT_g_Flag);
        ASSERT(parse_first({"-v"}) == OPT_v);
        ASSERT(parse_first({"-c"}) == OPT_c);
        ASSERT(parse_first({"-pedantic"}) == OPT_pedantic);
        ASSERT(parse_first({"--pedantic"}) == OPT_pedantic);
        ASSERT(parse_first({"-Wno-unused-variable"}) == OPT_W_Joined);
        ASSERT(parse_first({"-Xclang", "-ast-dump"}) == OPT_Xclang);
        ASSERT(parse_first({"-Wl,foo"}) == OPT_Wl_COMMA);
        ASSERT(parse_first({"-o", "out.o"}) == OPT_o);
        ASSERT(parse_first({"-omain.o"}) == OPT_o);
        ASSERT(parse_first({"-I", "/usr/include"}) == OPT_I);
        ASSERT(parse_first({"-x", "c++"}) == OPT_x);
    };

    ZEST_CASE(InputAndUnknown) {
        ASSERT(parse_first({"main.cpp"}) == OPT_INPUT);
        ASSERT(parse_first({"--clice-unknown-flag"}) == OPT_UNKNOWN);
    };

    ZEST_CASE(AliasAndDashDash) {
        ASSERT(parse_first({"--include-directory=/usr/include"}) == OPT_I);
        ASSERT(parse_first({"--language=c++"}) == OPT_x);
        ASSERT(parse_first({"--std=c++20"}) == OPT_std_EQ);

        std::vector<std::string> args = {"-I", "/usr/include", "--", "main.cpp"};
        auto options = kota::option::ParseOptions{.dash_dash_parsing = true};
        unsigned count = 0;
        for(auto& result: option::table().parse(args, options)) {
            if(result.has_value()) {
                ++count;
            }
        }
        ASSERT(count == 2u);
    };

    ZEST_CASE(ParseError) {
        std::vector<std::string> args = {"-o"};
        bool got_error = false;
        for(auto& result: option::table().parse(args)) {
            if(!result.has_value()) {
                got_error = true;
            }
        }
        EXPECT(got_error);
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

        ASSERT(parse_with_vis({"/DFOO"}, cl_vis) == OPT_D);
        ASSERT(parse_with_vis({"-DFOO"}, gcc_vis) == OPT_D);
        ASSERT(parse_with_vis({"-DFOO"}, cl_vis) == OPT_D);
        /// /D carries the DXC visibility bit besides CL; a Unix-driver mask must
        /// exclude both or /Data-style paths misparse.
        ASSERT(parse_with_vis({"/DFOO"}, gcc_vis) == OPT_INPUT);
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
        ASSERT(r1.size() == 2u);
        ASSERT(r1[0] == "-I");
        ASSERT(r1[1] == "/usr/include");

        auto r2 = roundtrip({"-DFOO=bar"});
        ASSERT(r2.size() == 2u);
        ASSERT(r2[0] == "-D");
        ASSERT(r2[1] == "FOO=bar");

        auto r3 = roundtrip({"-Wno-unused"});
        ASSERT(r3.size() == 1u);
        ASSERT(r3[0] == "-Wno-unused");

        auto r4 = roundtrip({"-std=c++20"});
        ASSERT(r4.size() == 1u);
        ASSERT(r4[0] == "-std=c++20");
    };

    ZEST_CASE(PrintArgv) {
        std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
        ASSERT(print_argv(args) == "clang++ -std=c++20 main.cpp");

        std::vector<const char*> empty = {};
        ASSERT(print_argv(empty) == "");

        std::vector<const char*> spaced = {"clang++", "-DFOO=hello world"};
        auto result = print_argv(spaced);
        EXPECT(llvm::StringRef(result).contains("\""));

        std::vector<const char*> escaped = {"clang++", "-DPATH=C:\\foo"};
        auto result2 = print_argv(escaped);
        EXPECT(llvm::StringRef(result2).contains("\""));
    };

};  // ZEST_SUITE(ArgumentParser)

}  // namespace

}  // namespace clice::testing
