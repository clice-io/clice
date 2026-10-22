module;

#include "modules/prelude.h"

module clice;

import :command.argument;
import :unittests.utils;

namespace clice::testing {

namespace {

using namespace option;

ZEST_SUITE(Argument) {

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
    auto cl_vis = family_visibility(CompilerFamily::ClangCL);
    auto gcc_vis = family_visibility(CompilerFamily::Clang);

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

ZEST_CASE(DriverFamily) {
    using enum CompilerFamily;

    ZEXPECT(driver_family("gcc") == GCC);
    ZEXPECT(driver_family("g++") == GCC);
    ZEXPECT(driver_family("cc") == GCC);
    ZEXPECT(driver_family("c++") == GCC);
    ZEXPECT(driver_family("gcc-13") == GCC);
    ZEXPECT(driver_family("g++-13.2") == GCC);
    ZEXPECT(driver_family("x86_64-linux-gnu-g++-14") == GCC);
    ZEXPECT(driver_family("arm-none-eabi-gcc") == GCC);

    ZEXPECT(driver_family("clang") == Clang);
    ZEXPECT(driver_family("clang++") == Clang);
    ZEXPECT(driver_family("clang.exe") == Clang);
    ZEXPECT(driver_family("clang++.exe") == Clang);
    ZEXPECT(driver_family("clang-20") == Clang);
    ZEXPECT(driver_family("clang-20.exe") == Clang);
    ZEXPECT(driver_family("clang++-21") == Clang);
    ZEXPECT(driver_family("clang-cl") == ClangCL);
    ZEXPECT(driver_family("clang-cl-20") == ClangCL);
    ZEXPECT(driver_family("clang-cl-20.exe") == ClangCL);

    ZEXPECT(driver_family("cl.exe") == MSVC);
    /// Windows casing is free-form: CL.exe is how msbuild spells it.
    ZEXPECT(driver_family("CL.exe") == MSVC);
    ZEXPECT(driver_family("CL.EXE") == MSVC);
    ZEXPECT(driver_family("Clang-Cl.exe") == ClangCL);
    ZEXPECT(driver_family("nvcc") == NVCC);
    ZEXPECT(driver_family("icx") == Intel);
    ZEXPECT(driver_family("icc") == Intel);
    ZEXPECT(driver_family("icpc") == Intel);
    ZEXPECT(driver_family("dpcpp") == Intel);

    ZEXPECT(driver_family("zig") == Zig);
    ZEXPECT(driver_family("zig.exe") == Zig);
};

ZEST_CASE(NameTarget) {
    ZEXPECT(name_target("riscv32-wch-elf-gcc") == "riscv32-wch-elf");
    ZEXPECT(name_target("/opt/arm/bin/arm-none-eabi-g++") == "arm-none-eabi");
    ZEXPECT(name_target("gcc").empty());
    ZEXPECT(name_target("clang++").empty());
    /// A prefix naming no architecture is no target.
    ZEXPECT(name_target("ccache-gcc").empty());
};

ZEST_CASE(NameTargetWindows, skip = !Windows) {
    /// A database written on Windows may name the driver this way.
    ZEXPECT(name_target(R"(C:/MounRiver/toolchain/gcc15/bin\riscv32-wch-elf-gcc.EXE)") ==
            "riscv32-wch-elf");
};

ZEST_CASE(EveryAliasRendersBack) {
    /// Every alias a GCC-style command can write renders back to itself:
    /// read again, the same option through the same alias, the same values.
    auto visibility = family_visibility(CompilerFamily::GCC);
    auto parse_options = kota::option::ParseOptions{.visibility = visibility};
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver strings(allocator);
    std::size_t checked = 0;
    for(auto& option: option::table().option_infos) {
        if(!option.alias_id || !(option.visibility & visibility)) {
            continue;
        }
        std::string name(option.prefixed_name);
        std::vector<std::string> written;
        switch(option.kind) {
            case kota::option::Kind::Flag: written = {name}; break;
            case kota::option::Kind::Joined: written = {name + "1"}; break;
            case kota::option::Kind::CommaJoined: written = {name + "a,b"}; break;
            case kota::option::Kind::Separate:
            case kota::option::Kind::JoinedOrSeparate: written = {name, "1"}; break;
            default: continue;
        }
        auto read = [&](std::vector<std::string>& args) -> std::optional<Arg> {
            std::optional<Arg> arg;
            for(auto& parsed: option::table().parse(args, parse_options)) {
                if(!parsed || arg) {
                    return std::nullopt;
                }
                arg = Arg{.opt_id = parsed->id, .alias = written_alias(*parsed)};
                std::vector<const char*> values;
                for(auto value: parsed->values) {
                    values.push_back(strings.save(llvm::StringRef(value)).data());
                }
                arg->values = llvm::ArrayRef(values).copy(allocator);
            }
            return arg;
        };
        // The parse may read the name as a longer option's: not one a
        // command writes through this alias.
        auto first = read(written);
        if(!first || first->alias != option.id) {
            continue;
        }
        std::vector<std::string> rendered;
        render_driver_arg(*first, CompilerFamily::GCC, [&](std::string_view fragment) {
            rendered.emplace_back(fragment);
        });
        auto again = read(rendered);
        ZEXPECT(again.has_value());
        if(again) {
            ZEXPECT(again->opt_id == first->opt_id);
            ZEXPECT(again->alias == first->alias);
            ZEXPECT(
                std::ranges::equal(again->values, first->values, [](const char* l, const char* r) {
                    return llvm::StringRef(l) == r;
                }));
        }
        checked += 1;
    }
    ZEXPECT(checked > 100U);
};

ZEST_CASE(DriverKeepsAlias) {
    /// GCC names its own way options clang knows as aliases: clang's own
    /// names reach a clang only.
    auto render = [](std::vector<std::string> args, CompilerFamily family) {
        std::vector<std::string> rendered;
        for(auto& parsed: option::table().parse(args)) {
            ZASSERT(parsed.has_value());
            std::vector<std::string> values(parsed->values.begin(), parsed->values.end());
            std::vector<const char*> pointers;
            for(auto& value: values) {
                pointers.push_back(value.c_str());
            }
            Arg arg{.opt_id = parsed->id, .alias = written_alias(*parsed), .values = pointers};
            render_driver_arg(arg, family, [&](std::string_view fragment) {
                rendered.emplace_back(fragment);
            });
        }
        return rendered;
    };
    using enum CompilerFamily;
    for(std::vector<std::string> args: {
            std::vector<std::string>{"-msmall-data-limit=8"},
            {"-flto"},
            {"-flto=auto"},
            {"-trigraphs"},
            {"-marm"},
            {"-mmacosx-version-min=11.0"},
            {"--sysroot", "/opt/sysroot"},
            {"-G", "8"},
            {"-std=c++20"}
    }) {
        ZEXPECT(render(args, GCC) == args);
    }
    std::vector<std::string> unaliased = {"-G", "8"};
    ZEXPECT(render({"-msmall-data-limit=8"}, NVCC) == unaliased);
};

};  // ZEST_SUITE(Argument)

ZEST_SUITE(Canonicalize) {

ZEST_CASE(StableForSameArgs) {
    std::vector<std::string> args = {"clang++", "-std=c++20", "-DFOO=1", "-I/usr/include"};
    ZASSERT(canonicalize(args) == canonicalize(args));
}

ZEST_CASE(CodegenFlagsIgnored) {
    // The point of the key: pure codegen flags must not change it.
    // Note -O* is deliberately NOT here: it defines
    // __OPTIMIZE__ and is kept by is_codegen_option() on purpose.
    auto base = canonicalize({"clang++", "-std=c++20", "-DFOO", "main.cpp"});
    ZASSERT(base == canonicalize({"clang++", "-g", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canonicalize({"clang++", "-gdwarf-4", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base ==
            canonicalize({"clang++", "-std=c++20", "-fomit-frame-pointer", "-DFOO", "main.cpp"}));
    ZASSERT(base ==
            canonicalize({"clang++", "-std=c++20", "-ffunction-sections", "-DFOO", "main.cpp"}));
    // Driver-ignored flags: bazel stamps a per-file -frandom-seed=<output>.
    ZASSERT(
        base ==
        canonicalize({"clang++", "-frandom-seed=out/main.o", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base == canonicalize({"clang++", "-std=c++20", "-fgcse", "-DFOO", "main.cpp"}));
}

ZEST_CASE(SemanticFlagsChangeKey) {
    // -D/-I/-std differences must produce different keys.
    auto base = canonicalize({"clang++", "-std=c++20", "-DFOO", "-I/a"});
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DBAR", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-I/b"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++17", "-DFOO", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-DBAR", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-UFOO", "-I/a"}));
    ZASSERT(base != canonicalize({"clang++", "-std=c++20", "-DFOO", "-isystem", "/sys", "-I/a"}));
}

ZEST_CASE(InputFileExcluded) {
    // The source file must stay out of the key so identical preambles
    // are shared across files.
    ZASSERT(canonicalize({"clang++", "-std=c++20", "a.cpp"}) ==
            canonicalize({"clang++", "-std=c++20", "b.cpp"}));
    // Absolute paths too (the CDB always carries absolute source paths).
    ZASSERT(canonicalize({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/a.cpp"}) ==
            canonicalize({"clang++", "-std=c++17", "-fsyntax-only", "/tmp/x/b.cpp"}));
    // cc1 commands carry per-file -main-file-name (injected by to_argv);
    // it labels diagnostics only and must stay out of the key.
    ZASSERT(
        canonicalize({"clang-20", "-cc1", "-main-file-name", "a.cpp", "-std=c++17", "/x/a.cpp"}) ==
        canonicalize({"clang-20", "-cc1", "-main-file-name", "b.cpp", "-std=c++17", "/x/b.cpp"}));
}

ZEST_CASE(OrderPreserved) {
    // -I/-D relative order is semantically significant; canonicalize
    // filters but never reorders.
    ZASSERT(canonicalize({"clang++", "-I/a", "-I/b"}) != canonicalize({"clang++", "-I/b", "-I/a"}));
    ZASSERT(canonicalize({"clang++", "-DFOO", "-UFOO"}) !=
            canonicalize({"clang++", "-UFOO", "-DFOO"}));
}

ZEST_CASE(DriverIncluded) {
    // clang vs clang++ changes language defaults.
    ZASSERT(canonicalize({"clang", "-DFOO"}) != canonicalize({"clang++", "-DFOO"}));
}

ZEST_CASE(SpellingNormalized) {
    // Alias spellings render identically after parsing.
    ZASSERT(canonicalize({"clang++", "-I", "/usr/include"}) ==
            canonicalize({"clang++", "-I/usr/include"}));
    ZASSERT(canonicalize({"clang++", "--include-directory=/usr/include"}) ==
            canonicalize({"clang++", "-I/usr/include"}));
}

ZEST_CASE(UnknownOptionKept) {
    // Unknown options might be semantic; dropping them could merge keys
    // that must differ.
    ZASSERT(canonicalize({"clang++", "-DFOO"}) !=
            canonicalize({"clang++", "-DFOO", "-fplugin-arg-x-y"}));
}

ZEST_CASE(WarningsChangeKey) {
    // Warnings affect diagnostics, which an LSP must reproduce.
    ZASSERT(canonicalize({"clang++", "-DFOO"}) != canonicalize({"clang++", "-Wall", "-DFOO"}));
}

};  // ZEST_SUITE(Canonicalize)

}  // namespace

}  // namespace clice::testing
