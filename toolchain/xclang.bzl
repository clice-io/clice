"""The C++ toolchain, generated from the clang on PATH (pixi's xclang).

One rule for Linux, macOS and Windows (MinGW), on rules_cc's Unix toolchain
config; rules_cc's own detection is off (.bazelrc). xclang's config files pick
sysroot, libc++, compiler-rt and linker, so the flags here are only Bazel's.
"""

def _run(rctx, args):
    res = rctx.execute(args)
    if res.return_code != 0:
        fail("%s failed: %s" % (" ".join([str(a) for a in args]), res.stderr))
    return res.stdout.strip()

def _impl(rctx):
    clang = rctx.which("clang")
    if not clang:
        fail("xclang's clang is not on PATH (pixi shell?)")
    os = rctx.os.name.lower()
    arm = rctx.os.arch in ("aarch64", "arm64")
    exe = ""
    if os.startswith("windows"):
        exe = ".exe"
        os_constraint, cpu, libc = "windows", "arm64_windows" if arm else "x64_windows", "mingw"
    elif os.startswith("mac"):
        os_constraint, cpu, libc = "macos", "darwin_arm64" if arm else "darwin_x86_64", "macosx"
    else:
        os_constraint, cpu, libc = "linux", "aarch64" if arm else "k8", "glibc"

    bindir = str(clang.dirname).replace("\\", "/")
    tool = lambda name: "%s/%s%s" % (bindir, name, exe)
    triple = _run(rctx, [tool("clang"), "-print-target-triple"])

    # Every header of the toolchain (libc++, compiler-rt, the sysroots) lives
    # under xclang's root.
    builtin_dirs = [str(clang.dirname.dirname).replace("\\", "/")]
    compile_flags = []
    link_flags = ["--driver-mode=g++"]
    opt_link_flags = ["-Wl,--gc-sections"]
    tool_paths = {
        "ar": tool("llvm-ar"),
        "cpp": tool("clang-cpp"),
        "dwp": tool("llvm-dwp"),
        "gcc": tool("clang"),
        "gcov": tool("llvm-cov"),
        "ld": tool("ld.lld"),
        "llvm-cov": tool("llvm-cov"),
        "llvm-profdata": tool("llvm-profdata"),
        "nm": tool("llvm-nm"),
        "objcopy": tool("llvm-objcopy"),
        "objdump": tool("llvm-objdump"),
        "strip": tool("llvm-strip"),
    }
    if os_constraint == "macos":
        # The SDK comes from Xcode. Its parent directory too: clang reports
        # SDKSettings.json under the versioned SDK name, not the symlink.
        sdk = _run(rctx, ["xcrun", "--show-sdk-path"])
        builtin_dirs += [sdk, sdk.rsplit("/", 1)[0]]
        compile_flags += ["-isysroot", sdk]
        link_flags += ["-isysroot", sdk]
        opt_link_flags = ["-Wl,-dead_strip"]
        tool_paths["libtool"] = tool("llvm-libtool-darwin")

    if exe:
        scanner = "deps_scanner_wrapper.bat"
        rctx.file(scanner, "@echo off\r\n\"%s\" -format=p1689 -- \"%s\" %%* > \"%%DEPS_SCANNER_OUTPUT_FILE%%\"\r\n" % (
            tool("clang-scan-deps"),
            tool("clang++"),
        ))
    else:
        scanner = "deps_scanner_wrapper.sh"
        rctx.file(scanner, "#!/bin/sh\nexec \"%s\" -format=p1689 -- \"%s\" \"$@\" > \"$DEPS_SCANNER_OUTPUT_FILE\"\n" % (
            tool("clang-scan-deps"),
            tool("clang++"),
        ), executable = True)
    tool_paths["cpp-module-deps-scanner"] = scanner

    constraints = json.encode(["@platforms//os:" + os_constraint, "@platforms//cpu:" + ("aarch64" if arm else "x86_64")])
    rctx.file("BUILD.bazel", """\
load("@rules_cc//cc/private/toolchain:unix_cc_toolchain_config.bzl", "cc_toolchain_config")  # buildifier: disable=bzl-visibility
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")

filegroup(name = "empty")

filegroup(
    name = "scanner",
    srcs = [{scanner}],
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
    coverage_compile_flags = ["-fprofile-instr-generate", "-fcoverage-mapping"],
    coverage_link_flags = ["-fprofile-instr-generate"],
    compile_flags = {compile_flags},
    cpu = {cpu},
    cxx_builtin_include_directories = {builtin_dirs},
    dbg_compile_flags = ["-g"],
    host_system_name = {triple},
    link_flags = {link_flags},
    opt_compile_flags = ["-O2", "-DNDEBUG", "-ffunction-sections", "-fdata-sections"],
    opt_link_flags = {opt_link_flags},
    target_libc = {libc},
    target_system_name = {triple},
    tool_paths = {tool_paths},
    toolchain_identifier = "xclang",
    # Keep __DATE__ and friends out of the outputs, as rules_cc's detection does.
    unfiltered_compile_flags = [
        "-no-canonical-prefixes",
        "-Wno-builtin-macro-redefined",
        "-D__DATE__=\\"redacted\\"",
        "-D__TIMESTAMP__=\\"redacted\\"",
        "-D__TIME__=\\"redacted\\"",
    ],
)

toolchain(
    name = "toolchain",
    exec_compatible_with = {constraints},
    target_compatible_with = {constraints},
    toolchain = ":cc",
    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",
)
""".format(
        scanner = json.encode(scanner),
        compile_flags = json.encode(compile_flags),
        cpu = json.encode(cpu),
        builtin_dirs = json.encode(builtin_dirs),
        triple = json.encode(triple),
        link_flags = json.encode(link_flags),
        opt_link_flags = json.encode(opt_link_flags),
        libc = json.encode(libc),
        tool_paths = json.encode(tool_paths),
        constraints = constraints,
    ))

xclang_toolchain = repository_rule(
    implementation = _impl,
    environ = ["PATH"],
    configure = True,
)
