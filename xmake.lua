set_xmakever("2.9.7")
set_project("clice")

set_allowedplats("windows", "linux", "macosx")
set_allowedmodes("debug", "release", "releasedbg")

option("enable_test", {default = true})
option("dev", {default = true})
option("release", {default = false})
option("llvm", {default = nil, description = "Specify pre-compiled llvm binary directory."})
option("ci", {default = false})

if has_config("dev") then
    set_policy("build.ccache", true)
    set_policy("compatibility.version", "3.0")
    if is_plat("windows") then
        set_runtimes("MD")
        if is_mode("debug") then
            print("Clice does not support build in debug mode with pre-compiled llvm binary on windows.\n"
                .."See https://github.com/clice-io/clice/issues/42 for more information.")
            os.raise()
        end
    elseif is_mode("debug") and is_plat("linux", "macosx") then
        set_policy("build.sanitizer.address", true)
    end
end

local libuv_require = "libuv"

if has_config("release") then
    set_policy("build.optimization.lto", true)
    set_policy("package.cmake_generator.ninja", true)

    if is_plat("windows") then
        set_runtimes("MT")
        -- workaround cmake
        libuv_require = "libuv[toolchains=clang-cl]"
    end

    includes("@builtin/xpack")
end

add_defines("TOML_EXCEPTIONS=0")
add_requires(libuv_require, "spdlog[header_only=n,std_format,noexcept]" ,"toml++")
add_requires("clice-llvm", {alias = "llvm"})

add_rules("mode.release", "mode.debug", "mode.releasedbg")
set_languages("c++23")
add_rules("clice_build_config")

target("clice-core")
    set_kind("$(kind)")
    add_files("src/**.cpp|Driver/*.cpp")
    add_includedirs("include", {public = true})

    add_packages("libuv", "spdlog", "toml++", {public = true})

    if is_mode("debug") then
        add_packages("llvm", {
            public = true,
            links = {
                "LLVMSupport",
                "LLVMFrontendOpenMP",
                "LLVMOption",
                "clangAST",
                "clangASTMatchers",
                "clangBasic",
                "clangDependencyScanning",
                "clangDriver",
                "clangFormat",
                "clangFrontend",
                "clangIndex",
                "clangLex",
                "clangSema",
                "clangSerialization",
                "clangTidy",
                "clangTidyUtils",
                -- ALL_CLANG_TIDY_CHECKS
                "clangTidyAndroidModule",
                "clangTidyAbseilModule",
                "clangTidyAlteraModule",
                "clangTidyBoostModule",
                "clangTidyBugproneModule",
                "clangTidyCERTModule",
                "clangTidyConcurrencyModule",
                "clangTidyCppCoreGuidelinesModule",
                "clangTidyDarwinModule",
                "clangTidyFuchsiaModule",
                "clangTidyGoogleModule",
                "clangTidyHICPPModule",
                "clangTidyLinuxKernelModule",
                "clangTidyLLVMModule",
                "clangTidyLLVMLibcModule",
                "clangTidyMiscModule",
                "clangTidyModernizeModule",
                "clangTidyObjCModule",
                "clangTidyOpenMPModule",
                "clangTidyPerformanceModule",
                "clangTidyPortabilityModule",
                "clangTidyReadabilityModule",
                "clangTidyZirconModule",
                "clangTooling",
                "clangToolingCore",
                "clangToolingInclusions",
                "clangToolingInclusionsStdlib",
                "clangToolingSyntax",
        }})
        on_config(function (target)
            local llvm_dynlib_dir = path.join(target:pkg("llvm"):installdir(), "lib")
            target:add("rpathdirs", llvm_dynlib_dir)
        end)
    elseif is_mode("release", "releasedbg") then
        add_packages("llvm", {public = true})
        add_ldflags("-Wl,--gc-sections")
    end

target("clice")
    set_kind("binary")
    add_files("src/Driver/clice.cc")

    add_deps("clice-core")

    on_config(function (target)
        local llvm_dir = target:dep("clice-core"):pkg("llvm"):installdir()
        target:add("installfiles", path.join(llvm_dir, "lib/clang/(**)"), {prefixdir = "lib/clang"})
    end)

    after_build(function (target)
        local res_dir = path.join(target:targetdir(), "lib/clang")
        if not os.exists(res_dir) then
            local llvm_dir = target:dep("clice-core"):pkg("llvm"):installdir()
            os.vcp(path.join(llvm_dir, "lib/clang"), res_dir)
        end
    end)

target("unit_tests")
    set_default(false)
    set_kind("binary")
    add_files("src/Driver/unit_tests.cc", "tests/unit/**.cpp")
    add_includedirs(".", {public = true})

    add_deps("clice-core")

    add_tests("default")

    after_load(function (target)
        target:set("runargs",
            "--test-dir=" .. path.absolute("tests/data"),
            "--resource-dir=" .. path.join(target:dep("clice-core"):pkg("llvm"):installdir(), "lib/clang/20")
        )
    end)

target("integration_tests")
    set_default(false)
    set_kind("phony")

    add_deps("clice")
    add_packages("llvm")

    add_tests("default")

    on_test(function (target, opt)
        import("lib.detect.find_tool")

        local uv = assert(find_tool("uv"), "uv not found!")
        local argv = {
            "run", "pytest",
            "--log-cli-level=INFO",
            "-s", "tests/integration",
            "--executable=" .. target:dep("clice"):targetfile(),
            "--resource-dir=" .. path.join(target:pkg("llvm"):installdir(), "lib/clang/20"),
        }
        local opt = {envs = envs, curdir = os.projectdir()}
        os.vrunv(uv.program, argv, opt)

        return true
    end)

rule("clice_build_config")
    on_load(function (target)
        target:add("cxflags", "-fno-rtti", {tools = {"clang", "clangxx", "gcc", "gxx"}})
        target:add("cxflags", "/GR-", {tools = {"clang_cl", "cl"}})
        -- Fix MSVC Non-standard preprocessor caused error C1189
        -- While compiling Command.cpp, MSVC won't expand Options macro correctly
        -- Output: D:\Desktop\code\clice\build\.packages\l\llvm\20.1.5\cc2aa9f1d09a4b71b6fa3bf0011f6387\include\clang/Driver/Options.inc(3590): error C2365: “clang::driver::options::OPT_”: redefinition; previous definition was 'enumerator'
        target:add("cxflags", "cl::/Zc:preprocessor")

        target:set("exceptions", "no-cxx")

        if target:is_plat("windows") and not target:toolchain("msvc") then
            target:set("toolset", "ar", "llvm-ar")
            if target:toolchain("clang-cl") then
                target:set("toolset", "ld", "lld-link")
                target:set("toolset", "sh", "lld-link")
            else
                target:add("ldflags", "-fuse-ld=lld-link")
            end
        elseif target:is_plat("linux") then
            -- gnu ld need to fix link order
            target:add("ldflags", "-fuse-ld=lld")
        end
        if has_config("ci") then
            target:add("cxxflags", "-DCLICE_CI_ENVIRONMENT")
        end
    end)

package("clice-llvm")
    if has_config("llvm") then
        set_sourcedir(get_config("llvm"))
    else
        on_source(function (package)
            import("core.base.json")

            local info = json.loadfile("./config/prebuilt-llvm.json")
            for _, info in ipairs(info) do
                if  get_config("mode") == info.build_type:lower()
                and get_config("plat") == info.platform:lower()
                and (info.is_lto == has_config("release")) then
                    package:add("urls", format("https://github.com/clice-io/llvm-binary/releases/download/%s/%s", info.version, info.filename))
                    package:add("versions", info.version, info.sha256)
                end
            end
        end)
    end

    if is_plat("linux", "macosx") then
        if is_mode("debug") then
            add_configs("shared", {description = "Build shared library.", default = true, type = "boolean", readonly = true})
        end
    end

    if is_plat("windows", "mingw") then
        add_syslinks("version", "ntdll")
    end

    on_install(function (package)
        if not package:config("shared") then
            package:add("defines", "CLANG_BUILD_STATIC")
        end

        os.vcp("bin", package:installdir())
        os.vcp("lib", package:installdir())
        os.vcp("include", package:installdir())
    end)

if has_config("release") then
    xpack("clice")
        if is_plat("windows") then
            set_formats("zip")
            set_extension(".7z")
        else
            set_formats("targz")
            set_extension(".tar.xz")
        end

        set_prefixdir("clice")

        add_targets("clice")
        add_installfiles(path.join(os.projectdir(), "docs/clice.toml"))

        on_load(function (package)
            local llvm_dir = package:target("clice"):dep("clice-core"):pkg("llvm"):installdir()
            package:add("installfiles", path.join(llvm_dir, "lib/clang/(**)"), {prefixdir = "lib/clang"})
        end)
end
