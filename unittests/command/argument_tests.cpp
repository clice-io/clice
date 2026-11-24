module clice;

import :command.argument;
import :unittests.utils;
import :unittests.utils.platform;

namespace clice::testing {

namespace {

using namespace clang_options;

ZEST_SUITE(Argument) {

unsigned parse_first(std::vector<std::string> args) {
    for(auto& result: clang_options::table().parse(args)) {
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

ZEST_CASE(ClassifyOptions) {
    /// What only the build reads leaves the command: a file each unit
    /// writes would set every command apart.
    for(auto id: {OPT_c,
                  OPT_MJ,
                  OPT_MV,
                  OPT_ftime_trace_EQ,
                  OPT_save_temps_EQ,
                  OPT__serialize_diags,
                  OPT_dumpdir,
                  OPT_Wa_COMMA,
                  OPT_Xassembler,
                  OPT__SLASH_Fa,
                  OPT__SLASH_Fe,
                  OPT__SLASH_Fi,
                  OPT__SLASH_Fm,
                  OPT__SLASH_FR,
                  OPT__SLASH_doc,
                  OPT__SLASH_sourceDependencies,
                  OPT__SLASH_showIncludes,
                  OPT__SLASH_showFilenames,
                  OPT_frandom_seed_EQ,
                  OPT_fsave_optimization_record,
                  OPT_foptimization_record_file_EQ,
                  OPT_gen_cdb_fragment_path}) {
        ZEXPECT(!classify_option(id, {}).has_value());
    }
    /// The precompiled files clice builds itself stay, for the identity.
    for(auto id: {OPT_include_pch, OPT_emit_pch, OPT_o, OPT_fmodule_output}) {
        ZEXPECT((classify_option(id, {}) == ArgClass::Precompiled));
    }
    /// -pthread defines _REENTRANT, and a specs file adds include
    /// directories (newlib-nano's).
    ZEXPECT((classify_option(OPT_pthread, {}) == ArgClass::Semantic));
    ZEXPECT((classify_option(OPT_specs_EQ, {}) == ArgClass::Semantic));
};

ZEST_CASE(LeftOutOptions) {
    /// What only code generation, linking, the header modules clice does
    /// not build, plugins and the backend read: what cc1 keeps in its
    /// CodeGenOptions, by driver and by cc1 names, the options the driver
    /// turns into those, the linker's group, the sanitizers' and profiles'
    /// details, how diagnostics print, and cl's own.
    for(auto id: {OPT_fno_plt,
                  OPT_faddrsig,
                  OPT_fstack_clash_protection,
                  OPT_mrelocation_model,
                  OPT_funwind_tables_EQ,
                  OPT_fno_strict_aliasing,
                  OPT_fomit_frame_pointer,
                  OPT_fvectorize,
                  OPT_mno_red_zone,
                  OPT_Wl_COMMA,
                  OPT_shared,
                  OPT_mfix_cortex_a53_835769,
                  OPT_fdebug_prefix_map_EQ,
                  OPT_fmodule_name_EQ,
                  OPT_fmodule_map_file,
                  OPT_fmodules_strict_decluse,
                  OPT_fmodules_cache_path,
                  OPT_fimplicit_module_maps,
                  OPT_fplugin_EQ,
                  OPT_moutline_atomics,
                  OPT_fsanitize_recover_EQ,
                  OPT_fprofile_sample_use_EQ,
                  OPT_fno_common,
                  OPT_fstack_usage,
                  OPT_fcoverage_mapping,
                  OPT_ftest_coverage,
                  OPT_fwhole_program_vtables,
                  OPT_ftrivial_auto_var_init,
                  OPT_fvisibility_inlines_hidden,
                  OPT_fcolor_diagnostics,
                  OPT_fmessage_length_EQ,
                  OPT__SLASH_GL,
                  OPT__SLASH_guard,
                  OPT__SLASH_bigobj}) {
        ZEXPECT(!classify_option(id, {}).has_value());
    }
};

ZEST_CASE(KeptOptions) {
    /// What looks like code generation but predefines macros, answers
    /// __has_feature, or sets what Sema reads: the optimization level
    /// (__OPTIMIZE__), position independence (__PIC__), the stack
    /// protector (__SSP__), fast math (__FAST_MATH__), the exception
    /// model cc1 keeps in its CodeGenOptions (__SEH__), CFI directives
    /// (__GCC_HAVE_DWARF2_CFI_ASM), the instrumentation profile
    /// (__LLVM_INSTR_PROFILE_GENERATE), control-flow protection (__CET__),
    /// Darwin's -static (__STATIC__), the sanitizers and the coverage one,
    /// LTO, which -fsanitize=cfi requires, trapping math, which strict FP
    /// evaluation follows, cl's runtime checks, and what round out those:
    /// MIPS's MSA (__mips_msa), the LTO unit splitting and cross-DSO CFI
    /// -fsanitize=cfi checks and expands by, unwind tables
    /// (__GCC_HAVE_DWARF2_CFI_ASM), the runtime library AArch64's function
    /// multiversioning follows, cl's /Zl (_VC_NODEFAULTLIB), ASan's field
    /// padding and the ignore lists exempting types from it, which change a
    /// record's layout, the options undoing others that stay, and debug
    /// information (__GCC_HAVE_DWARF2_CFI_ASM in a C file without unwind
    /// tables).
    for(auto id: {OPT_O,
                  OPT_fPIC,
                  OPT_fstack_protector_strong,
                  OPT_ffast_math,
                  OPT_fno_exceptions,
                  OPT_funsigned_char,
                  OPT_fno_inline,
                  OPT_exception_model,
                  OPT_fdwarf2_cfi_asm,
                  OPT_fprofile_instr_generate,
                  OPT_fprofile_instrument_EQ,
                  OPT_fcf_protection_EQ,
                  OPT_static,
                  OPT_fsanitize_EQ,
                  OPT_fsanitize_coverage,
                  OPT_fsanitize_coverage_type,
                  OPT_fxray_instrument,
                  OPT_flto_EQ,
                  OPT_fvisibility_EQ,
                  OPT_ftrapping_math,
                  OPT_mtune_EQ,
                  OPT__sysroot_EQ,
                  OPT__SLASH_RTC,
                  OPT_mmsa,
                  OPT_fsplit_lto_unit,
                  OPT_fsanitize_cfi_cross_dso,
                  OPT_fno_asynchronous_unwind_tables,
                  OPT_rtlib_EQ,
                  OPT_fms_omit_default_lib,
                  OPT_fsanitize_address_field_padding,
                  OPT_fsanitize_ignorelist_EQ,
                  OPT_fsanitize_ignore_for_ubsan_feature_EQ,
                  OPT_fvisibility_global_new_delete_EQ,
                  OPT_femit_all_decls,
                  OPT_fgnu_inline_asm,
                  OPT_finline,
                  OPT_g_Flag,
                  OPT_funified_lto,
                  OPT_falloc_token_max_EQ,
                  OPT_mllvm,
                  OPT_fuse_ld_EQ,
                  OPT_nostdlib}) {
        ZEXPECT((classify_option(id, {}) == ArgClass::Semantic));
    }
    ZEXPECT((classify_option(OPT_W_Joined, {}) == ArgClass::Diagnostics));
    ZEXPECT((classify_option(OPT_pedantic, {}) == ArgClass::Diagnostics));
    ZEXPECT((classify_option(OPT_I, {}) == ArgClass::UserContent));
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
    auto options = cli::ParseOptions{.dash_dash_parsing = true};
    unsigned count = 0;
    for(auto& result: clang_options::table().parse(args, options)) {
        if(result.has_value()) {
            count += 1;
        }
    }
    ZASSERT(count == 2u);
};

ZEST_CASE(ParseError) {
    std::vector<std::string> args = {"-o"};
    bool got_error = false;
    for(auto& result: clang_options::table().parse(args)) {
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
        auto options = cli::ParseOptions{.dash_dash_parsing = true, .visibility = vis};
        for(auto& result: clang_options::table().parse(args, options)) {
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
        for(auto& result: clang_options::table().parse(input)) {
            if(!result.has_value())
                continue;
            auto cb = [&](std::string_view s) {
                rendered.emplace_back(s);
            };
            clang_options::table().render(*result, cb);
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
    ZEXPECT(driver_family("icx") == (Windows ? ClangCL : Intel));
    ZEXPECT(driver_family("icx.exe") == (Windows ? ClangCL : Intel));
    ZEXPECT(driver_family("icx-cl") == ClangCL);
    ZEXPECT(driver_family("icx-cl.exe") == ClangCL);
    ZEXPECT(driver_family("dpcpp-cl") == ClangCL);
    ZEXPECT(driver_family("icpx") == Intel);
    ZEXPECT(driver_family("icpx.exe") == Intel);
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
    auto parse_options = cli::ParseOptions{.visibility = visibility};
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver strings(allocator);
    std::size_t checked = 0;
    for(auto& option: clang_options::table().option_infos) {
        if(!option.alias_id || !(option.visibility & visibility)) {
            continue;
        }
        std::string name(option.prefixed_name);
        std::vector<std::string> written;
        switch(option.kind) {
            case cli::OptionKind::Flag: written = {name}; break;
            case cli::OptionKind::Joined: written = {name + "1"}; break;
            case cli::OptionKind::CommaJoined: written = {name + "a,b"}; break;
            case cli::OptionKind::Separate:
            case cli::OptionKind::JoinedOrSeparate: written = {name, "1"}; break;
            default: continue;
        }
        auto read = [&](std::vector<std::string>& args) -> std::optional<Arg> {
            std::optional<Arg> arg;
            for(auto& parsed: clang_options::table().parse(args, parse_options)) {
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
            ZEXPECT(ranges::equal(again->values, first->values, [](const char* l, const char* r) {
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
        for(auto& parsed: clang_options::table().parse(args)) {
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
            std::vector<std::string>{"-flto"},
            {"-flto=auto"},
            {"-trigraphs"},
            {"-marm"},
            {"-mmacosx-version-min=11.0"},
            {"--sysroot", "/opt/sysroot"},
            {"-std=c++20"}
    }) {
        ZEXPECT(render(args, GCC) == args);
    }
    std::vector<std::string> unaliased = {"-ftrigraphs"};
    ZEXPECT(render({"-trigraphs"}, NVCC) == unaliased);
    /// A second prefix of an option names no alias.
    std::vector<std::string> standard = {"-std=c++20"};
    ZEXPECT(render({"--std=c++20"}, GCC) == standard);
};

};  // ZEST_SUITE(Argument)

ZEST_SUITE(Canonicalize) {

ZEST_CASE(StableForSameArgs) {
    std::vector<std::string> args = {"clang++", "-std=c++20", "-DFOO=1", "-I/usr/include"};
    ZASSERT(canonicalize(args) == canonicalize(args));
}

ZEST_CASE(CodegenFlagsIgnored) {
    // The point of the key: pure codegen flags must not change it.
    // Note -O* is deliberately NOT here: it defines __OPTIMIZE__.
    auto base = canonicalize({"clang++", "-std=c++20", "-DFOO", "main.cpp"});
    ZASSERT(base == canonicalize({"clang++", "-fno-plt", "-std=c++20", "-DFOO", "main.cpp"}));
    ZASSERT(base ==
            canonicalize({"clang++", "-fno-strict-aliasing", "-std=c++20", "-DFOO", "main.cpp"}));
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
            canonicalize({"clang++", "-DFOO", "--clice-unknown-flag"}));
}

ZEST_CASE(WarningsChangeKey) {
    // Warnings affect diagnostics, which an LSP must reproduce.
    ZASSERT(canonicalize({"clang++", "-DFOO"}) != canonicalize({"clang++", "-Wall", "-DFOO"}));
}

};  // ZEST_SUITE(Canonicalize)

}  // namespace

}  // namespace clice::testing
