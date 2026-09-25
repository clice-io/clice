#!/usr/bin/env python3
"""Build the LLVM package clice links against.

Two configure/build passes share one install prefix. The runtimes pass builds
a static, hermetic libc++ (libc++abi merged in outside Windows) from the same
llvm-project tree; the LLVM pass then builds clang and the libraries clice
links (COMPONENTS) against that libc++, with no code-generation target, and
lib/cmake/clice-llvm/config.cmake records the toolchain configuration for
cmake/llvm.cmake to check against.
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

# The MC layer of X86 only (target info, instruction tables, assembly
# parser): clang parses MS-style `__asm {}` blocks through it. No CodeGen.
X86_MC_COMPONENTS = [
    "LLVMX86Info",
    "LLVMX86Desc",
    "LLVMX86AsmParser",
    "LLVMCodeGenTypes",
    "LLVMMCDisassembler",
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
        self.mingw = self.triple.endswith("-w64-mingw32")
        self.msvc = IS_WINDOWS and not self.mingw
        self.asan = self.mode == "Debug" and not IS_WINDOWS and not self.mingw
        self.pgo_instrument: bool = args.pgo_instrument
        self.pgo_instrument_fe: bool = args.pgo_instrument_fe
        self.pgo_profile: Path | None = (
            Path(args.pgo_profile).resolve() if args.pgo_profile else None
        )
        self.clang_only: bool = args.clang_only
        self.pgo_strip: bool = args.pgo_strip_prefix
        self.pgo_remap: Path | None = Path(args.pgo_remap).resolve() if args.pgo_remap else None
        self.msvc_ob2: bool = args.msvc_ob2
        self.default_triple_override: str | None = args.default_triple
        self.x86_mc: bool = args.x86_mc
        # A profile only matches code compiled the same way, so the
        # instrumented build takes the release configuration of the final
        # (LTO) build it trains: no assertions, no libc++ hardening.
        self.release_config = (
            self.lto or self.pgo_instrument or self.pgo_instrument_fe or bool(self.pgo_profile)
        )
        self.assertions = self.mode == "Debug" or not self.release_config

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
        if self.msvc:
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
        args = [
            f"-DCMAKE_TOOLCHAIN_FILE={self.toolchain_file.as_posix()}",
            "-DLLVM_USE_LINKER=lld",
        ]
        if self.mingw:
            # LLVM defaults this to ON for MinGW; clice and its tests expect
            # the MSVC package's backslash-preferred paths.
            args.append("-DLLVM_WINDOWS_PREFER_FORWARD_SLASH=OFF")
        return args

    def debug_info_args(self) -> list[str]:
        # Function names and line tables are all the symbolizers need; the
        # type and variable information of a full -g is most of the archive.
        if self.msvc:
            # /Ob1 (CMake's MSVC default) inlines only functions declared
            # inline; --msvc-ob2 lets the inliner decide, like -O2 elsewhere.
            inline = "/Ob2" if self.msvc_ob2 else "/Ob1"
            relwithdebinfo = f"/O2 {inline} /DNDEBUG -gcodeview -gline-tables-only"
            debug = "/Ob0 /Od -gcodeview -gline-tables-only"
        else:
            relwithdebinfo = "-O2 -gline-tables-only -DNDEBUG"
            debug = "-gline-tables-only"
        args = []
        for lang in ("C", "CXX"):
            args.append(f"-DCMAKE_{lang}_FLAGS_RELWITHDEBINFO={relwithdebinfo}")
            args.append(f"-DCMAKE_{lang}_FLAGS_DEBUG={debug}")
        return args

    def pgo_name_flags(self) -> str:
        """Profile names of static functions carry the TU's absolute path;
        strip everything up to the llvm-project root so a profile trained
        in one checkout (host, repository) matches another."""
        flags = ""
        if self.pgo_strip:
            flags += f" -mllvm -static-func-strip-dirname-prefix={len(self.root.parts)}"
        if self.pgo_remap and self.pgo_profile:
            flags += f" -fprofile-remapping-file={self.pgo_remap.as_posix()}"
        return flags

    def common_args(self, cxx_flags: str) -> list[str]:
        args = [
            "-G",
            "Ninja",
            f"-DCMAKE_BUILD_TYPE={self.mode}",
            f"-DCMAKE_INSTALL_PREFIX={self.install_prefix.as_posix()}",
            f"-DCMAKE_C_FLAGS=-w{self.driver_flags()}{self.target_flags()}{self.pgo_name_flags()}",
            f"-DCMAKE_CXX_FLAGS={cxx_flags}{self.driver_flags()}{self.target_flags()}{self.pgo_name_flags()}",
            # The archive triple doubles as the default: without a native
            # backend LLVM would leave it empty, and clang would then have no
            # target for compile commands that do not spell one. The mingw
            # package is the Windows package, and what Windows users compile
            # targets MSVC unless their command says otherwise.
            f"-DLLVM_DEFAULT_TARGET_TRIPLE={self.default_triple()}",
            f"-DLLVM_ENABLE_LTO={'Thin' if self.lto else 'OFF'}",
            *self.compiler_args(),
            *self.debug_info_args(),
        ]
        if self.ccache:
            args += ["-DLLVM_CCACHE_BUILD=ON", f"-DCCACHE_PROGRAM={self.ccache}"]
        if self.target_triple:
            args.append(f"-DCLICE_TARGET_TRIPLE={self.target_triple}")
        return args

    def default_triple(self) -> str:
        if self.default_triple_override:
            return self.default_triple_override
        if self.mingw:
            return self.triple.replace("-w64-mingw32", "-pc-windows-msvc")
        return self.triple

    # --------------------------------------------------------------- runtimes

    def hardening_mode(self) -> str:
        # The debug mode's comparator validation (every sort comparison is
        # evaluated twice) made the Debug clice nine times slower on its
        # semantic pass; extensive keeps every bounds check without it.
        if self.mode == "Debug":
            return "extensive"
        return "none" if self.release_config else "fast"

    def build_runtimes(self) -> None:
        """The libc++ is not sanitizer-instrumented even in the ASan variant:
        the instrumented build needs a compiler-rt lookup that fails on Apple
        for static-only builds, and container-overflow detection is not worth
        a patch."""
        build_dir = self.build_dir / "runtimes"
        args = self.common_args("-w") + [
            "-DLLVM_ENABLE_RUNTIMES=" + ("libcxx" if self.msvc else "libcxxabi;libcxx"),
            "-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF",
            "-DLLVM_INCLUDE_TESTS=OFF",
            "-DLIBCXX_ENABLE_SHARED=OFF",
            "-DLIBCXX_ENABLE_STATIC=ON",
            "-DLIBCXX_HERMETIC_STATIC_LIBRARY=ON",
            f"-DLIBCXX_HARDENING_MODE={self.hardening_mode()}",
            "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
            "-DLIBCXX_INCLUDE_TESTS=OFF",
        ]
        if self.msvc:
            # Upstream compiles libc++ with _CRT_STDIO_ISO_WIDE_SPECIFIERS, and
            # the UCRT's detect_mismatch then forces that mode on every object
            # linked with it; the ISO mode changes what %s means in the wide
            # printf family, which libuv relies on. The library only formats
            # numbers with it, so it is built mode-agnostic instead (the
            # "static library" mode of corecrt_stdio_config.h). Target flags
            # come after the definitions on the command line, so the
            # undefine wins.
            args.append(
                "-DLIBCXX_ADDITIONAL_COMPILE_FLAGS="
                "/U_CRT_STDIO_ISO_WIDE_SPECIFIERS;/D_CRT_STDIO_ARBITRARY_WIDE_SPECIFIERS"
            )
        else:
            args += [
                "-DLIBCXX_CXX_ABI=libcxxabi",
                "-DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON",
                "-DLIBCXXABI_ENABLE_SHARED=OFF",
                "-DLIBCXXABI_HERMETIC_STATIC_LIBRARY=ON",
                "-DLIBCXXABI_USE_LLVM_UNWINDER=OFF",
                "-DLIBCXXABI_INCLUDE_TESTS=OFF",
            ]

        print(f"\nConfiguring runtimes in {build_dir}...")
        run(["cmake", "-S", self.root / "runtimes", "-B", build_dir] + args)
        print("\nInstalling runtimes...")
        run(["cmake", "--build", build_dir, "--target", "install"])

    # ------------------------------------------------------------------- llvm

    def libcxx_args(self) -> tuple[str, str]:
        """Compile and link flags that make the runtimes pass's libc++ the
        standard library of the LLVM pass."""
        include = (self.install_prefix / "include/c++/v1").as_posix()
        lib = (self.install_prefix / "lib").as_posix()
        if self.msvc:
            # clang-cl has no -nostdinc++; the MSVC STL headers sit in the
            # INCLUDE directories, which -isystem precedes. libc++.lib is a
            # plain linker input, not a /DEFAULTLIB: lld-link reads the
            # command-line inputs and the objects' directive libraries
            # (libcmt) before any /DEFAULTLIB, and libcmt also defines
            # std::nothrow, so as a default library libc++ would lose that
            # symbol to libcmt and then duplicate it when new_helpers.cpp.obj
            # is pulled in for __throw_bad_alloc. On the vcruntime ABI libc++
            # leaves std::set_new_handler to the MSVC STL (libcpmt), which
            # nothing auto-links once its headers are shadowed; it duplicates
            # libc++'s exception_ptr definitions, so it stays a default
            # library, searched after everything else.
            return (
                f"-w /clang:-isystem{include}",
                f"{lib}/libc++.lib /DEFAULTLIB:libcpmt.lib",
            )
        # -D on the command line replaces the toolchain file's *_INIT linker
        # flags, so lld is repeated here. The archive is named outright: a
        # -stdlib=libc++ would take any libc++ an earlier -L (LDFLAGS, a
        # host toolchain) puts in the search path, and lld does not care
        # where on the command line an archive sits.
        return (
            f"-w -nostdinc++ -isystem {include}",
            f"-fuse-ld=lld{self.driver_flags()} -nostdlib++ {lib}/libc++.a",
        )

    def llvm_args(self) -> list[str]:
        cxx_flags, linker_flags = self.libcxx_args()
        args = self.common_args(cxx_flags) + [
            f"-DCMAKE_EXE_LINKER_FLAGS={linker_flags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={linker_flags}",
            f"-DCMAKE_MODULE_LINKER_FLAGS={linker_flags}",
            *(["-DLLVM_USE_SANITIZER=Address"] if self.asan else []),
            "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
            # No backend is built, but clang/lib/Headers generates arm_neon.h,
            # arm_sve.h and riscv_vector.h only when their target is listed;
            # install-distribution never reaches the backends themselves. The
            # clang compiler itself (--clang-only) needs a real backend.
            "-DLLVM_TARGETS_TO_BUILD="
            + ("X86;AArch64;ARM;RISCV" if self.clang_only or self.x86_mc else "AArch64;ARM;RISCV"),
            f"-DLLVM_DISTRIBUTION_COMPONENTS={';'.join(COMPONENTS + (X86_MC_COMPONENTS if self.x86_mc else []))}",
            f"-DLLVM_ENABLE_ASSERTIONS={'ON' if self.assertions else 'OFF'}",
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
        if self.pgo_instrument:
            args.append("-DLLVM_BUILD_INSTRUMENTED=IR")
        if self.pgo_instrument_fe:
            args.append("-DLLVM_BUILD_INSTRUMENTED=Frontend")
        if self.pgo_profile:
            args.append(f"-DLLVM_PROFDATA_FILE={self.pgo_profile.as_posix()}")
        if self.target_triple:
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
        runtimes_cache = read_cmake_cache(
            self.build_dir / "runtimes" / "CMakeCache.txt"
        )
        compiler = compiler_info(build_dir)
        entries = {
            "LLVM_VERSION": llvm_version_of(self.root),
            "COMPILER_ID": compiler.get("CMAKE_CXX_COMPILER_ID", ""),
            "COMPILER_VERSION": compiler.get("CMAKE_CXX_COMPILER_VERSION", ""),
            "TARGET_TRIPLE": self.triple,
            "BUILD_TYPE": self.mode,
            "LTO": "ON" if self.lto else "OFF",
            "PGO": (
                "instrumented"
                if self.pgo_instrument
                else self.pgo_profile.name if self.pgo_profile else "OFF"
            ),
            "ASAN": "ON" if self.asan else "OFF",
            "ASSERTIONS": "ON" if self.assertions else "OFF",
            "RTTI": "OFF",
            "STDLIB": "libc++",
            "LIBCXX_ABI_VERSION": runtimes_cache.get("LIBCXX_ABI_VERSION", ""),
            "LIBCXX_HARDENING_MODE": self.hardening_mode(),
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
        "--pgo-instrument",
        action="store_true",
        help="Instrument LLVM for IR PGO (LLVM_BUILD_INSTRUMENTED=IR)",
    )
    parser.add_argument(
        "--pgo-instrument-fe",
        action="store_true",
        help="Instrument LLVM for frontend (clang AST-based) PGO instead",
    )
    parser.add_argument(
        "--pgo-profile",
        help="Optimize LLVM with this .profdata (LLVM_PROFDATA_FILE)",
    )
    parser.add_argument(
        "--clang-only",
        action="store_true",
        help="Build the clang compiler (with the X86 backend) instead of the package",
    )
    parser.add_argument(
        "--msvc-ob2",
        action="store_true",
        help="Build the MSVC-target RelWithDebInfo package with /Ob2 instead of /Ob1",
    )
    parser.add_argument(
        "--pgo-remap",
        help="Profile remapping file (-fprofile-remapping-file), for names mangled differently per target",
    )
    parser.add_argument(
        "--pgo-strip-prefix",
        action="store_true",
        help="Name static functions in profiles relative to the llvm-project root",
    )
    parser.add_argument(
        "--x86-mc",
        action="store_true",
        help="Also ship the X86 MC layer (info, descriptions, assembly parser) for MS inline asm",
    )
    parser.add_argument(
        "--default-triple",
        help="LLVM_DEFAULT_TARGET_TRIPLE, when it should differ from the target triple",
    )
    parser.add_argument(
        "--configure-only",
        action="store_true",
        help="Build the runtimes and configure LLVM, print the size of the build plan, then stop before building LLVM",
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
        build.build_runtimes()
        build_dir = build.configure_llvm()
        if args.clang_only:
            print("\nBuilding 'clang'...")
            run(["cmake", "--build", build_dir, "--target", "clang"])
            print(f"\nSuccess! clang built in: {build_dir / 'bin'}")
            return
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
