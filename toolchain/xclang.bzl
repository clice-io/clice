"""A C++ toolchain over xclang's clang targeting MinGW, with C++20 modules."""

_BUILD = """
load("@@//toolchain:cc_toolchain_config.bzl", "cc_toolchain_config")
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")

filegroup(name = "empty")

filegroup(
    name = "scanner",
    srcs = ["deps_scanner_wrapper.bat"],
)

cc_toolchain(
    name = "cc",
    all_files = ":scanner",
    ar_files = ":empty",
    as_files = ":empty",
    compiler_files = ":scanner",
    dwp_files = ":empty",
    linker_files = ":empty",
    objcopy_files = ":empty",
    strip_files = ":empty",
    supports_param_files = 1,
    toolchain_config = ":config",
)

cc_toolchain_config(
    name = "config",
    abi_libc_version = "local",
    abi_version = "local",
    compiler = "clang",
    cpu = "x64_windows",
    cxx_builtin_include_directories = ["{root}"],
    dbg_compile_flags = ["-g"],
    host_system_name = "x86_64-w64-mingw32",
    # The C driver links like g++ (libc++, libunwind) only in g++ mode.
    link_flags = ["--driver-mode=g++", "-fuse-ld=lld"],
    opt_compile_flags = ["-O2", "-DNDEBUG", "-ffunction-sections", "-fdata-sections"],
    opt_link_flags = ["-Wl,--gc-sections"],
    coverage_compile_flags = ["-fprofile-instr-generate", "-fcoverage-mapping"],
    coverage_link_flags = ["-fprofile-instr-generate"],
    supports_start_end_lib = False,
    target_libc = "mingw",
    target_system_name = "x86_64-w64-mingw32",
    tool_paths = {{
        "gcc": "{bin}/clang{exe}",
        "cpp": "{bin}/clang-cpp{exe}",
        "ar": "{bin}/llvm-ar{exe}",
        "ld": "{bin}/ld.lld{exe}",
        "nm": "{bin}/llvm-nm{exe}",
        "objcopy": "{bin}/llvm-objcopy{exe}",
        "objdump": "{bin}/llvm-objdump{exe}",
        "strip": "{bin}/llvm-strip{exe}",
        "gcov": "{bin}/llvm-cov{exe}",
        "dwp": "{bin}/llvm-dwp{exe}",
        "llvm-cov": "{bin}/llvm-cov{exe}",
        "llvm-profdata": "{bin}/llvm-profdata{exe}",
        "cpp-module-deps-scanner": "deps_scanner_wrapper.bat",
    }},
    toolchain_identifier = "xclang-mingw",
)

toolchain(
    name = "toolchain",
    exec_compatible_with = ["@platforms//os:windows", "@platforms//cpu:x86_64"],
    target_compatible_with = ["@platforms//os:windows", "@platforms//cpu:x86_64"],
    toolchain = ":cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
"""

def _impl(rctx):
    clang = rctx.which("clang")
    if not clang:
        fail("xclang's clang is not on PATH")
    windows = rctx.os.name.lower().startswith("windows")
    exe = ".exe" if windows else ""
    bindir = str(clang.dirname).replace("\\", "/")
    root = str(clang.dirname.dirname).replace("\\", "/")
    rctx.file("deps_scanner_wrapper.bat", "@echo off\r\n\"{bin}/clang-scan-deps{exe}\" -format=p1689 -- \"{bin}/clang++{exe}\" %* > \"%DEPS_SCANNER_OUTPUT_FILE%\"\r\n".format(bin = bindir, exe = exe))
    rctx.file("BUILD.bazel", _BUILD.format(bin = bindir, root = root, exe = exe))

xclang_toolchain = repository_rule(
    implementation = _impl,
    environ = ["PATH"],
)
