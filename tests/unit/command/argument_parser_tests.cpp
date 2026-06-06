#include "test/test.h"
#include "command/argument_parser.h"

namespace clice::testing {

namespace {

using namespace option;

TEST_SUITE(ArgumentParser) {

unsigned parse_first(std::vector<std::string> args) {
    for(auto& result: option::table().parse(args)) {
        if(result.has_value()) {
            return result->id;
        }
    }
    return OPT_INVALID;
}

TEST_CASE(ParseOptionID) {
    ASSERT_EQ(parse_first({"-g"}), OPT_g_Flag);
    ASSERT_EQ(parse_first({"-v"}), OPT_v);
    ASSERT_EQ(parse_first({"-c"}), OPT_c);
    ASSERT_EQ(parse_first({"-pedantic"}), OPT_pedantic);
    ASSERT_EQ(parse_first({"--pedantic"}), OPT_pedantic);
    ASSERT_EQ(parse_first({"-Wno-unused-variable"}), OPT_W_Joined);
    ASSERT_EQ(parse_first({"-Xclang", "-ast-dump"}), OPT_Xclang);
    ASSERT_EQ(parse_first({"-Wl,foo"}), OPT_Wl_COMMA);
    ASSERT_EQ(parse_first({"-o", "out.o"}), OPT_o);
    ASSERT_EQ(parse_first({"-omain.o"}), OPT_o);
    ASSERT_EQ(parse_first({"-I", "/usr/include"}), OPT_I);
    ASSERT_EQ(parse_first({"-x", "c++"}), OPT_x);
};

TEST_CASE(InputAndUnknown) {
    ASSERT_EQ(parse_first({"main.cpp"}), OPT_INPUT);
    ASSERT_EQ(parse_first({"--clice-unknown-flag"}), OPT_UNKNOWN);
};

TEST_CASE(AliasAndDashDash) {
    ASSERT_EQ(parse_first({"--include-directory=/usr/include"}), OPT_I);
    ASSERT_EQ(parse_first({"--language=c++"}), OPT_x);
    ASSERT_EQ(parse_first({"--std=c++20"}), OPT_std_EQ);

    std::vector<std::string> args = {"-I", "/usr/include", "--", "main.cpp"};
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true};
    unsigned count = 0;
    for(auto& result: option::table().parse(args, options)) {
        if(result.has_value()) {
            ++count;
        }
    }
    ASSERT_EQ(count, 2u);
};

TEST_CASE(ParseError) {
    std::vector<std::string> args = {"-o"};
    bool got_error = false;
    for(auto& result: option::table().parse(args)) {
        if(!result.has_value()) {
            got_error = true;
        }
    }
    EXPECT_TRUE(got_error);
};

TEST_CASE(CLVisibility) {
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

    ASSERT_EQ(parse_with_vis({"/DFOO"}, cl_vis), OPT_D);
    ASSERT_EQ(parse_with_vis({"-DFOO"}, gcc_vis), OPT_D);
    ASSERT_EQ(parse_with_vis({"-DFOO"}, cl_vis), OPT_D);
};

TEST_CASE(RenderRoundTrip) {
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
    ASSERT_EQ(r1.size(), 2u);
    ASSERT_EQ(r1[0], "-I");
    ASSERT_EQ(r1[1], "/usr/include");

    auto r2 = roundtrip({"-DFOO=bar"});
    ASSERT_EQ(r2.size(), 2u);
    ASSERT_EQ(r2[0], "-D");
    ASSERT_EQ(r2[1], "FOO=bar");

    auto r3 = roundtrip({"-Wno-unused"});
    ASSERT_EQ(r3.size(), 1u);
    ASSERT_EQ(r3[0], "-Wno-unused");

    auto r4 = roundtrip({"-std=c++20"});
    ASSERT_EQ(r4.size(), 1u);
    ASSERT_EQ(r4[0], "-std=c++20");
};

TEST_CASE(PrintArgv) {
    std::vector<const char*> args = {"clang++", "-std=c++20", "main.cpp"};
    ASSERT_EQ(print_argv(args), "clang++ -std=c++20 main.cpp");

    std::vector<const char*> empty = {};
    ASSERT_EQ(print_argv(empty), "");

    std::vector<const char*> spaced = {"clang++", "-DFOO=hello world"};
    auto result = print_argv(spaced);
    EXPECT_TRUE(llvm::StringRef(result).contains("\""));

    std::vector<const char*> escaped = {"clang++", "-DPATH=C:\\foo"};
    auto result2 = print_argv(escaped);
    EXPECT_TRUE(llvm::StringRef(result2).contains("\""));
};

};  // TEST_SUITE(ArgumentParser)

}  // namespace

}  // namespace clice::testing
