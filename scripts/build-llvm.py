#!/usr/bin/env python3
"""Build the LLVM package clice links against.

Only the libraries clice links (COMPONENTS) are built and installed, no
code-generation target is configured, and lib/cmake/clice-llvm/config.cmake
records the toolchain configuration for cmake/llvm.cmake to check against.
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path


MODE_MAP = {
    "debug": "Debug",
    "release": "Release",
    "relwithdebinfo": "RelWithDebInfo",
    "releasedbg": "RelWithDebInfo",
}

# The transitive closure of what cmake/llvm.cmake links. Every library named
# here is installed and exported; a library missing from the closure fails the
# LLVM configure (the export set requires it), so the list stays complete.
COMPONENTS = [
    "clangAPINotes",
    "clangAST",
    "clangASTMatchers",
    "clangAnalysis",
    "clangAnalysisFlowSensitive",
    "clangAnalysisFlowSensitiveModels",
    "clangAnalysisLifetimeSafety",
    "clangBasic",
    "clangDependencyScanning",
    "clangDriver",
    "clangEdit",
    "clangFormat",
    "clangFrontend",
    "clangIncludeCleaner",
    "clangIndex",
    "clangLex",
    "clangOptions",
    "clangParse",
    "clangRewrite",
    "clangScalableStaticAnalysisAnalyses",
    "clangScalableStaticAnalysisCore",
    "clangScalableStaticAnalysisFrontend",
    "clangScalableStaticAnalysisSourceTransformation",
    "clangSema",
    "clangSerialization",
    "clangSupport",
    "clangTidy",
    "clangTidyAbseilModule",
    "clangTidyAlteraModule",
    "clangTidyAndroidModule",
    "clangTidyBoostModule",
    "clangTidyBugproneModule",
    "clangTidyCERTModule",
    "clangTidyConcurrencyModule",
    "clangTidyCppCoreGuidelinesModule",
    "clangTidyDarwinModule",
    "clangTidyFuchsiaModule",
    "clangTidyGoogleModule",
    "clangTidyLLVMLibcModule",
    "clangTidyLLVMModule",
    "clangTidyLinuxKernelModule",
    "clangTidyMiscModule",
    "clangTidyModernizeModule",
    "clangTidyObjCModule",
    "clangTidyOpenMPModule",
    "clangTidyPerformanceModule",
    "clangTidyPortabilityModule",
    "clangTidyReadabilityModule",
    "clangTidyUtils",
    "clangTidyZirconModule",
    "clangTooling",
    "clangToolingCore",
    "clangToolingInclusions",
    "clangToolingInclusionsStdlib",
    "clangToolingRefactoring",
    "clangToolingSyntax",
    "clangTransformer",
    "clangUnifiedSymbolResolution",
    "LLVMAggressiveInstCombine",
    "LLVMAnalysis",
    "LLVMAsmParser",
    "LLVMBinaryFormat",
    "LLVMBitReader",
    "LLVMBitstreamReader",
    "LLVMCore",
    "LLVMDebugInfoBTF",
    "LLVMDebugInfoCodeView",
    "LLVMDebugInfoDWARF",
    "LLVMDebugInfoDWARFLowLevel",
    "LLVMDebugInfoGSYM",
    "LLVMDebugInfoMSF",
    "LLVMDebugInfoPDB",
    "LLVMDemangle",
    "LLVMFrontendAtomic",
    "LLVMFrontendDirective",
    "LLVMFrontendHLSL",
    "LLVMFrontendOffloading",
    "LLVMFrontendOpenMP",
    "LLVMIRReader",
    "LLVMInstCombine",
    "LLVMMC",
    "LLVMMCParser",
    "LLVMObject",
    "LLVMObjectYAML",
    "LLVMOption",
    "LLVMPlugins",
    "LLVMProfileData",
    "LLVMRemarks",
    "LLVMScalarOpts",
    "LLVMSupport",
    "LLVMSymbolize",
    "LLVMTargetParser",
    "LLVMTextAPI",
    "LLVMTransformUtils",
    "LLVMWindowsDriver",
    "llvm-headers",
    "clang-headers",
    "clang-tidy-headers",
    "clang-resource-headers",
    "cmake-exports",
    "clang-cmake-exports",
]

SEMA_PRIVATE_HEADERS = ["CoroutineStmtBuilder.h", "TypeLocBuilder.h", "TreeTransform.h"]

IS_WINDOWS = sys.platform == "win32"
IS_DARWIN = sys.platform == "darwin"


def run(args: list) -> None:
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.check_call([str(a) for a in args])


def find_ccache() -> str | None:
    env = os.environ.get("CCACHE_PROGRAM") or os.environ.get("CCACHE")
    program = shutil.which(env) if env else shutil.which("ccache")
    if not program and env and Path(env).exists():
        program = env
    return Path(program).as_posix() if program else None


def host_triple() -> str:
    """The archive triple of this machine, spelled as cmake/llvm.cmake spells it."""
    machine = platform.machine().lower()
    if machine in ("arm64", "aarch64"):
        arch = "aarch64"
    elif machine in ("x86_64", "amd64"):
        arch = "x86_64"
    else:
        sys.exit(f"Unsupported processor: {platform.machine()}")
    if IS_WINDOWS:
        return f"{arch}-pc-windows-msvc"
    if IS_DARWIN:
        return f"{arch}-apple-darwin"
    return f"{arch}-unknown-linux-gnu"


def llvm_version_of(source_root: Path) -> str:
    text = (source_root / "cmake/Modules/LLVMVersion.cmake").read_text()
    parts = [
        re.search(rf"set\(LLVM_VERSION_{k} (\d+)", text)
        for k in ("MAJOR", "MINOR", "PATCH")
    ]
    return ".".join(m.group(1) for m in parts if m)


def compiler_info(build_dir: Path) -> dict[str, str]:
    """The compiler CMake detected: it is not in the cache, only in its own file."""
    values: dict[str, str] = {}
    for path in build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"):
        for key in ("CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION"):
            match = re.search(rf'set\({key} "([^"]*)"\)', path.read_text())
            if match:
                values[key] = match.group(1)
    return values


def read_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for line in path.read_text().splitlines():
        match = re.match(r"^([^#/].*?):[A-Z]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


class Build:
    def __init__(
        self, args: argparse.Namespace, project_root: Path, toolchain_file: Path
    ):
        self.root = project_root
        self.toolchain_file = toolchain_file
        self.mode = MODE_MAP[args.mode.strip().lower()]
        self.lto = args.lto == "ON"
        self.target_triple: str | None = args.target_triple
        self.triple = args.target_triple or host_triple()
        self.asan = self.mode == "Debug" and not IS_WINDOWS
        self.assertions = self.mode == "Debug" or not self.lto

        if args.build_dir:
            build_dir = Path(args.build_dir)
            if not build_dir.is_absolute():
                build_dir = project_root / build_dir
        else:
            build_dir = (
                project_root / f"build-{self.mode.lower()}{'-lto' if self.lto else ''}"
            )
        self.build_dir = build_dir
        self.install_prefix = build_dir.parent / f"{build_dir.name}-install"
        self.ccache = find_ccache()

    # ------------------------------------------------------------------ flags

    def target_flags(self) -> str:
        return (
            f" --target={self.target_triple}"
            if self.target_triple and IS_WINDOWS
            else ""
        )

    # -DCMAKE_<LANG>_FLAGS replaces the toolchain file's *_INIT value, so the
    # conda config-file opt-out is repeated here.
    def driver_flags(self) -> str:
        return " --no-default-config" if IS_DARWIN else ""

    def compiler_args(self) -> list[str]:
        if IS_WINDOWS:
            return [
                "-DCMAKE_C_COMPILER=clang-cl",
                "-DCMAKE_CXX_COMPILER=clang-cl",
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
                # LLVM opts into CMP0141, under which CMake appends -Zi (full
                # debug info) to every RelWithDebInfo/Debug target after the
                # per-config flags, overriding -gline-tables-only. Empty means
                # CMake adds no debug format flag at all.
                "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=",
                "-DLLVM_USE_LINKER=lld-link",
            ]
        return [
            f"-DCMAKE_TOOLCHAIN_FILE={self.toolchain_file.as_posix()}",
            "-DLLVM_USE_LINKER=lld",
        ]

    def debug_info_args(self) -> list[str]:
        # Function names and line tables are all the symbolizers need; the
        # type and variable information of a full -g is most of the archive.
        if IS_WINDOWS:
            relwithdebinfo = "/O2 /Ob1 /DNDEBUG -gcodeview -gline-tables-only"
            debug = "/Ob0 /Od -gcodeview -gline-tables-only"
        else:
            relwithdebinfo = "-O2 -gline-tables-only -DNDEBUG"
            debug = "-gline-tables-only"
        args = []
        for lang in ("C", "CXX"):
            args.append(f"-DCMAKE_{lang}_FLAGS_RELWITHDEBINFO={relwithdebinfo}")
            args.append(f"-DCMAKE_{lang}_FLAGS_DEBUG={debug}")
        return args

    def stdlib(self) -> str:
        if IS_WINDOWS:
            return "msvc-stl"
        if IS_DARWIN:
            return "libc++"
        return "libstdc++"

    def llvm_args(self) -> list[str]:
        args = [
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={self.mode}",
            f"-DCMAKE_INSTALL_PREFIX={self.install_prefix.as_posix()}",
            f"-DCMAKE_C_FLAGS=-w{self.driver_flags()}{self.target_flags()}",
            f"-DCMAKE_CXX_FLAGS=-w{self.driver_flags()}{self.target_flags()}",
            *self.compiler_args(),
            *self.debug_info_args(),
            "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
            # No backend is built, but clang/lib/Headers generates arm_neon.h,
            # arm_sve.h and riscv_vector.h only when their target is listed;
            # install-distribution never reaches the backends themselves.
            "-DLLVM_TARGETS_TO_BUILD=AArch64;ARM;RISCV",
            # Without a native backend LLVM leaves the default triple empty,
            # and clang would then have no target for compile commands that
            # do not spell one. The archive triple doubles as the default.
            f"-DLLVM_DEFAULT_TARGET_TRIPLE={self.triple}",
            f"-DLLVM_DISTRIBUTION_COMPONENTS={';'.join(COMPONENTS)}",
            f"-DLLVM_ENABLE_ASSERTIONS={'ON' if self.assertions else 'OFF'}",
            f"-DLLVM_ENABLE_LTO={'Thin' if self.lto else 'OFF'}",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DLLVM_ENABLE_RTTI=OFF",
            "-DLLVM_ENABLE_DIA_SDK=OFF",
            "-DLLVM_ENABLE_ZLIB=OFF",
            "-DLLVM_ENABLE_ZSTD=OFF",
            "-DLLVM_ENABLE_LIBXML2=OFF",
            "-DLLVM_ENABLE_BINDINGS=OFF",
            "-DLLVM_ENABLE_IDE=OFF",
            "-DLLVM_ENABLE_Z3_SOLVER=OFF",
            "-DLLVM_ENABLE_LIBEDIT=OFF",
            "-DLLVM_ENABLE_LIBPFM=OFF",
            "-DLLVM_ENABLE_OCAMLDOC=OFF",
            "-DLLVM_ENABLE_PLUGINS=OFF",
            "-DLLVM_INCLUDE_UTILS=OFF",
            "-DLLVM_INCLUDE_TESTS=OFF",
            "-DLLVM_INCLUDE_EXAMPLES=OFF",
            "-DLLVM_INCLUDE_BENCHMARKS=OFF",
            "-DLLVM_INCLUDE_DOCS=OFF",
            "-DLLVM_INCLUDE_TOOLS=ON",
            "-DLLVM_BUILD_UTILS=OFF",
            "-DLLVM_BUILD_TOOLS=OFF",
            "-DLLVM_BUILD_LLVM_C_DYLIB=OFF",
            "-DLLVM_LINK_LLVM_DYLIB=OFF",
            "-DLLVM_DISABLE_ASSEMBLY_FILES=ON",
            "-DLLVM_PARALLEL_LINK_JOBS=1",
            "-DCMAKE_JOB_POOL_LINK=console",
            "-DCLANG_BUILD_TOOLS=OFF",
            "-DCLANG_INCLUDE_DOCS=OFF",
            "-DCLANG_INCLUDE_TESTS=OFF",
            "-DCLANG_TOOL_CLANG_IMPORT_TEST_BUILD=OFF",
            "-DCLANG_TOOL_CLANG_LINKER_WRAPPER_BUILD=OFF",
            "-DCLANG_TOOL_C_INDEX_TEST_BUILD=OFF",
            "-DCLANG_TOOL_LIBCLANG_BUILD=OFF",
            "-DCLANG_ENABLE_CLANGD=OFF",
            # clice's cmake/clang-tidy-config.h disables both; the package
            # must agree so clangTidy drops the analyzer and the MPI module.
            "-DCLANG_TIDY_ENABLE_STATIC_ANALYZER=OFF",
            "-DCLANG_TIDY_ENABLE_QUERY_BASED_CUSTOM_CHECKS=OFF",
        ]
        if self.asan:
            args.append("-DLLVM_USE_SANITIZER=Address")
        if self.ccache:
            args += ["-DLLVM_CCACHE_BUILD=ON", f"-DCCACHE_PROGRAM={self.ccache}"]
        if self.target_triple:
            args.append(f"-DCLICE_TARGET_TRIPLE={self.target_triple}")
            args.append(f"-DLLVM_HOST_TRIPLE={self.target_triple}")
            if not IS_DARWIN:
                args.append(f"-DLLVM_NATIVE_TOOL_DIR={self.build_native_tools()}")
        return args

    def build_native_tools(self) -> Path:
        """Cross builds need tablegen binaries that run on the build machine.

        macOS runs the x86_64 tools through Rosetta; Linux and Windows need a
        separate native configure of just the tablegen targets.
        """
        native_dir = self.build_dir / "native-tools"
        native_dir.mkdir(parents=True, exist_ok=True)
        args = [
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
            "-DLLVM_TARGETS_TO_BUILD=",
            "-DLLVM_DISABLE_ASSEMBLY_FILES=ON",
            "-DCMAKE_C_FLAGS=-w",
            "-DCMAKE_CXX_FLAGS=-w",
        ]
        if IS_WINDOWS:
            args += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        else:
            args += ["-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]

        print(f"\nConfiguring native host tools in {native_dir}...")
        run(["cmake", "-S", self.root / "llvm", "-B", native_dir] + args)
        for tool in ["llvm-tblgen", "llvm-min-tblgen", "clang-tblgen"]:
            run(["cmake", "--build", native_dir, "--target", tool])
        try:
            run(
                [
                    "cmake",
                    "--build",
                    native_dir,
                    "--target",
                    "clang-tidy-confusable-chars-gen",
                ]
            )
        except subprocess.CalledProcessError:
            print("  clang-tidy-confusable-chars-gen not available, skipping.")
        return native_dir / "bin"

    # ------------------------------------------------------------------- llvm

    def configure_llvm(self) -> Path:
        sema = self.root / "clang/lib/Sema"
        missing = [
            header for header in SEMA_PRIVATE_HEADERS if not (sema / header).exists()
        ]
        if missing:
            sys.exit(f"Private Sema headers missing in {sema}: {', '.join(missing)}")

        build_dir = self.build_dir / "llvm"
        print(f"\nConfiguring LLVM in {build_dir}...")
        run(["cmake", "-S", self.root / "llvm", "-B", build_dir] + self.llvm_args())
        return build_dir

    def install_llvm(self, build_dir: Path) -> None:
        print("\nBuilding 'install-distribution' target...")
        run(["cmake", "--build", build_dir, "--target", "install-distribution"])

        print("\nCopying internal Sema headers...")
        dest = self.install_prefix / "include/clang/Sema"
        dest.mkdir(parents=True, exist_ok=True)
        for header in SEMA_PRIVATE_HEADERS:
            shutil.copy(self.root / "clang/lib/Sema" / header, dest / header)

    # --------------------------------------------------------------- manifest

    def write_manifest(self, build_dir: Path) -> None:
        """Record the configuration a consumer must match, for cmake/llvm.cmake."""
        cache = read_cmake_cache(build_dir / "CMakeCache.txt")
        compiler = compiler_info(build_dir)
        entries = {
            "LLVM_VERSION": llvm_version_of(self.root),
            "COMPILER_ID": compiler.get("CMAKE_CXX_COMPILER_ID", ""),
            "COMPILER_VERSION": compiler.get("CMAKE_CXX_COMPILER_VERSION", ""),
            "TARGET_TRIPLE": self.triple,
            "BUILD_TYPE": self.mode,
            "LTO": "ON" if self.lto else "OFF",
            "ASAN": "ON" if self.asan else "OFF",
            "ASSERTIONS": "ON" if self.assertions else "OFF",
            "RTTI": "OFF",
            "STDLIB": self.stdlib(),
            "MSVC_RUNTIME_LIBRARY": cache.get("CMAKE_MSVC_RUNTIME_LIBRARY", ""),
            "OSX_DEPLOYMENT_TARGET": cache.get("CMAKE_OSX_DEPLOYMENT_TARGET", ""),
        }
        manifest = self.install_prefix / "lib/cmake/clice-llvm/config.cmake"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        lines = [f'set(CLICE_LLVM_{key} "{value}")' for key, value in entries.items()]
        manifest.write_text("\n".join(lines) + "\n")
        print(f"\nWrote {manifest}")
        for line in lines:
            print(f"  {line}")

    # ---------------------------------------------------------------- summary

    def print_size_summary(self) -> None:
        lib_dir = self.install_prefix / "lib"
        sizes = sorted(
            ((p, p.stat().st_size) for p in lib_dir.rglob("*") if p.is_file()),
            key=lambda x: x[1],
            reverse=True,
        )
        total = sum(size for _, size in sizes)
        print(f"\nLibrary size summary under {lib_dir}:")
        print(f"  Total: {total / 1048576:.1f} MB across {len(sizes)} files")
        for path, size in sizes[:40]:
            print(
                f"  {size / 1048576:>8.1f} MB  {path.relative_to(self.install_prefix)}"
            )


def print_build_plan(build_dir: Path) -> None:
    commands = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "commands", "install-distribution"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    compiles = sum(
        1
        for line in commands
        if re.search(r"\s[-/]c\s", line) and re.search(r"\.(cpp|cc|c)\b", line)
    )
    print(f"\ninstall-distribution: {len(commands)} build steps, {compiles} compiles")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build the LLVM package clice links against."
    )
    parser.add_argument(
        "--llvm-src",
        help="Path to llvm-project source root (defaults to current working directory)",
    )
    parser.add_argument(
        "--mode", default="Release", help="Build mode (default: Release)"
    )
    parser.add_argument(
        "--lto",
        default="OFF",
        type=lambda s: s.upper(),
        choices=["ON", "OFF"],
        help="Enable ThinLTO (default: OFF)",
    )
    parser.add_argument(
        "--build-dir", help="Build directory (relative to the source root or absolute)"
    )
    parser.add_argument(
        "--target-triple",
        help="Cross-compilation target triple (e.g. x86_64-apple-darwin, aarch64-unknown-linux-gnu, aarch64-pc-windows-msvc)",
    )
    parser.add_argument(
        "--configure-only",
        action="store_true",
        help="Configure LLVM and print the size of the build plan, then stop before building",
    )
    args = parser.parse_args()

    if args.mode.strip().lower() not in MODE_MAP:
        parser.error(
            f"Invalid mode '{args.mode}'. Choose from Debug, Release, RelWithDebInfo."
        )

    repo_root = Path(__file__).resolve().parent.parent
    toolchain_file = repo_root / "cmake" / "toolchain.cmake"
    if not toolchain_file.exists():
        sys.exit(f"Error: toolchain file not found at {toolchain_file}")

    project_root = (
        Path(args.llvm_src).expanduser().resolve() if args.llvm_src else Path.cwd()
    )
    if not (project_root / "llvm" / "CMakeLists.txt").exists():
        sys.exit(f"Error: {project_root} is not the root of an llvm-project checkout.")
    os.chdir(project_root)

    if args.target_triple:
        # conda's cross activation exports host-platform -isystem/-L flags that
        # would leak into the target build (and into the native tools build).
        for var in ["LIBRARY_PATH", "LDFLAGS", "CFLAGS", "CXXFLAGS", "CPPFLAGS"]:
            os.environ.pop(var, None)

    build = Build(args, project_root, toolchain_file)
    print(f"mode={build.mode}")
    print(f"lto={args.lto}")
    print(f"triple={build.triple}{'' if build.target_triple else ' (native)'}")
    print(f"root={project_root} (LLVM {llvm_version_of(project_root)})")
    print(f"build_dir={build.build_dir}")
    print(f"install_prefix={build.install_prefix}")
    print(f"ccache={build.ccache or '(none)'}")

    build.build_dir.mkdir(parents=True, exist_ok=True)
    try:
        build_dir = build.configure_llvm()
        if args.configure_only:
            print_build_plan(build_dir)
            build.write_manifest(build_dir)
            return
        build.install_llvm(build_dir)
        build.write_manifest(build_dir)
    except subprocess.CalledProcessError as error:
        sys.exit(f"Command failed with exit code {error.returncode}: {error.cmd}")

    build.print_size_summary()
    print(f"\nSuccess! Artifacts installed to: {build.install_prefix}")


if __name__ == "__main__":
    main()
