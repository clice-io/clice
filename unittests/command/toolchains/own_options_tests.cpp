module clice;

import :command.argument;
import :command.toolchains.own_options;
import :unittests.utils;

namespace clice::testing {

namespace {

using enum CompilerFamily;

/// The options `args` read as for `family`, their ids and values.
std::vector<std::pair<unsigned, std::vector<std::string>>> read(std::vector<std::string> args,
                                                                CompilerFamily family) {
    std::vector<std::pair<unsigned, std::vector<std::string>>> read;
    auto options =
        cli::ParseOptions{.dash_dash_parsing = true, .visibility = family_visibility(family)};
    for(auto& parsed: parse_arguments(args, family, options)) {
        ZASSERT(parsed);
        read.push_back({
            parsed->id,
            {parsed->values.begin(), parsed->values.end()}
        });
    }
    return read;
}

ZEST_SUITE(OwnOptions) {

ZEST_CASE(BuildOutputsDropped) {
    // Files of each unit the build writes, which clang's table does not
    // know: GCC's module mapper and scan output, Apple's index unit output.
    auto args = read({"-fmodule-mapper=a.cppm.o.modmap",
                      "-fdeps-file=a.ddi",
                      "-aux-info",
                      "a.aux",
                      "-index-unit-output-path",
                      "/out/a.o",
                      "-DX"},
                     GCC);
    ZASSERT(args.size() == 1u);
    ZEXPECT(args[0].first == clang_options::OPT_D);
}

ZEST_CASE(ClModulesDropped) {
    // What cl's modules ask of a unit, as CMake's response files spell it
    // and as MSBuild does.
    auto args = read({"-interface",
                      "/internalPartition",
                      "-ifcOutput",
                      "a.ifc",
                      "/ifcSearchDir",
                      "ifc",
                      "/ifcOutputb.ifc",
                      "-ifcMap",
                      "map.toml",
                      "/stdIfcDir",
                      "std",
                      "-scanDependencies",
                      "a.json",
                      "-DX"},
                     MSVC);
    ZASSERT(args.size() == 1u);
    ZEXPECT(args[0].second == std::vector<std::string>{"X"});
}

ZEST_CASE(IntelOpenMP) {
    // icpx's OpenMP is clang's; icx in cl mode reads /Qiopenmp and ignores
    // the GCC-style spellings.
    auto gcc_style = read({"-qopenmp", "-fiopenmp", "-qno-openmp", "-qopenmp-simd"}, Intel);
    ZASSERT(gcc_style.size() == 4u);
    ZEXPECT(gcc_style[0].first == clang_options::OPT_fopenmp);
    ZEXPECT(gcc_style[1].first == clang_options::OPT_fopenmp);
    ZEXPECT(gcc_style[2].first == clang_options::OPT_fno_openmp);
    ZEXPECT(gcc_style[3].first == clang_options::OPT_fopenmp_simd);

    auto cl = read({"/Qiopenmp", "-Qiopenmp", "/Qopenmp-", "-qopenmp"}, ClangCL);
    ZASSERT(cl.size() == 4u);
    ZEXPECT(cl[0].first == clang_options::OPT_fopenmp);
    ZEXPECT(cl[1].first == clang_options::OPT_fopenmp);
    ZEXPECT(cl[2].first == clang_options::OPT_fno_openmp);
    ZEXPECT(cl[3].first == clang_options::OPT_UNKNOWN);
}

ZEST_CASE(SYCLSpelling) {
    // icx-cl's /fsycl, which clang's cl mode takes for an input; a path
    // elsewhere.
    auto cl = read({"/fsycl", "/c"}, ClangCL);
    ZASSERT(cl.size() == 2u);
    ZEXPECT(cl[0].first == clang_options::OPT_fsycl);
    ZEXPECT(cl[1].first == clang_options::OPT_c);

    auto gcc = read({"/fsycl"}, GCC);
    ZASSERT(gcc.size() == 1u);
    ZEXPECT(gcc[0].first == clang_options::OPT_INPUT);
}

ZEST_CASE(EndOfOptions) {
    // After `--` an own spelling is an input too.
    auto args = read({"-qopenmp", "--", "-qopenmp"}, Intel);
    ZASSERT(args.size() == 2u);
    ZEXPECT(args[0].first == clang_options::OPT_fopenmp);
    ZEXPECT(args[1].first == clang_options::OPT_INPUT);
}

};  // ZEST_SUITE(OwnOptions)

}  // namespace

}  // namespace clice::testing
