#!/usr/bin/env python3
import sys
import subprocess
import shutil
import argparse
import os
from pathlib import Path


def normalize_mode(value: str) -> str:
    mapping = {
        "debug": "Debug",
        "release": "Release",
        "relwithdebinfo": "RelWithDebInfo",
        "releasedbg": "RelWithDebInfo",
    }
    key = value.strip().lower()
    if key in mapping:
        return mapping[key]
    raise argparse.ArgumentTypeError(
        f"Invalid mode '{value}'. Choose from Debug, Release, RelWithDebInfo."
    )


def main():
    parser = argparse.ArgumentParser(
        description="Build LLVM with specific configurations."
    )
    parser.add_argument(
        "--llvm-src",
        help="Path to llvm-project source root (defaults to current working directory)",
    )
    parser.add_argument(
        "--mode",
        default="Release",
        type=normalize_mode,
        choices=["Debug", "Release", "RelWithDebInfo"],
        help="Build mode (default: Release)",
    )
    parser.add_argument(
        "--lto",
        default="OFF",
        type=lambda s: s.upper(),
        choices=["ON", "OFF"],
        help="Enable LTO (default: OFF)",
    )
    parser.add_argument(
        "--build-dir",
        help="Custom build directory (relative to project root or absolute)",
    )

    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    toolchain_file = repo_root / "cmake" / "toolchain.cmake"
    if not toolchain_file.exists():
        print(f"Error: toolchain file not found at {toolchain_file}")
        sys.exit(1)

    if args.llvm_src:
        project_root = Path(args.llvm_src).expanduser().resolve()
    else:
        project_root = Path.cwd()
    os.chdir(project_root)

    if not (project_root / "llvm" / "CMakeLists.txt").exists():
        print(f"Error: Could not find 'llvm/CMakeLists.txt' in {project_root}")
        print("Please run this script from the root of the llvm-project repository.")
        sys.exit(1)

    lto_enabled = args.lto == "ON"
    mode_for_dir = args.mode.lower()

    if args.build_dir:
        build_dir = Path(args.build_dir)
        if not build_dir.is_absolute():
            build_dir = project_root / build_dir
    else:
        build_dir = f"build-{mode_for_dir}"
        if lto_enabled:
            build_dir += "-lto"
        build_dir = project_root / build_dir
    install_prefix = build_dir.parent / f"{build_dir.name}-install"

    print("--- Configuration ---")
    print(f"Mode:           {args.mode}")
    print(f"LTO:            {args.lto}")
    print(f"Root:           {project_root}")
    print(f"Build Dir:      {build_dir}")
    print(f"Install Prefix: {install_prefix}")
    print(f"Toolchain:      {toolchain_file}")
    print("---------------------")

    llvm_distribution_components = [
        "LLVMDemangle",
        "LLVMSupport",
        "LLVMCore",
        "LLVMOption",
        "LLVMBinaryFormat",
        "LLVMMC",
        "LLVMMCParser",
        "LLVMObject",
        "LLVMProfileData",
        "LLVMBitReader",
        "LLVMBitstreamReader",
        "LLVMRemarks",
        "LLVMObjectYAML",
        "LLVMAggressiveInstCombine",
        "LLVMInstCombine",
        "LLVMIRReader",
        "LLVMTextAPI",
        "LLVMSymbolize",
        "LLVMDebugInfoDWARF",
        "LLVMDebugInfoDWARFLowLevel",
        "LLVMDebugInfoCodeView",
        "LLVMDebugInfoGSYM",
        "LLVMDebugInfoPDB",
        "LLVMDebugInfoBTF",
        "LLVMDebugInfoMSF",
        "LLVMAsmParser",
        "LLVMTargetParser",
        "LLVMTransformUtils",
        "LLVMAnalysis",
        "LLVMScalarOpts",
        "LLVMFrontendHLSL",
        "LLVMFrontendOpenMP",
        "LLVMFrontendOffloading",
        "LLVMFrontendAtomic",
        "LLVMFrontendDirective",
        "LLVMWindowsDriver",
        "clangIndex",
        "clangAPINotes",
        "clangAST",
        "clangASTMatchers",
        "clangBasic",
        "clangDriver",
        "clangFormat",
        "clangFrontend",
        "clangLex",
        "clangParse",
        "clangSema",
        "clangSerialization",
        "clangRewrite",
        "clangAnalysis",
        "clangEdit",
        "clangSupport",
        "clangStaticAnalyzerCore",
        "clangStaticAnalyzerFrontend",
        "clangTidy",
        "clangTidyUtils",
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
        "clangToolingRefactoring",
        "clangTransformer",
        "clangCrossTU",
        "clangAnalysisFlowSensitive",
        "clangAnalysisFlowSensitiveModels",
        "clangStaticAnalyzerCheckers",
        "clangIncludeCleaner",
        "llvm-headers",
        "clang-headers",
        "clang-tidy-headers",
        "clang-resource-headers",
    ]

    components_joined = ";".join(llvm_distribution_components)
    cmake_args = [
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain_file.as_posix()}",
        f"-DCMAKE_INSTALL_PREFIX={install_prefix}",
        "-DCMAKE_C_FLAGS=-w",
        "-DCMAKE_CXX_FLAGS=-w",
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
        "-DLLVM_BUILD_UTILS=OFF",
        "-DLLVM_BUILD_TOOLS=OFF",
        "-DCLANG_BUILD_TOOLS=OFF",
        "-DCLANG_INCLUDE_DOCS=OFF",
        "-DCLANG_INCLUDE_TESTS=OFF",
        "-DCLANG_TOOL_CLANG_IMPORT_TEST_BUILD=OFF",
        "-DCLANG_TOOL_CLANG_LINKER_WRAPPER_BUILD=OFF",
        "-DCLANG_TOOL_C_INDEX_TEST_BUILD=OFF",
        "-DCLANG_TOOL_LIBCLANG_BUILD=OFF",
        "-DCLANG_ENABLE_CLANGD=OFF",
        "-DLLVM_BUILD_LLVM_C_DYLIB=OFF",
        "-DLLVM_LINK_LLVM_DYLIB=OFF",
        "-DLLVM_ENABLE_RTTI=OFF",
        # Enable features
        "-DLLVM_INCLUDE_TOOLS=ON",
        "-DLLVM_PARALLEL_LINK_JOBS=1",
        "-DCMAKE_JOB_POOL_LINK=console",
        "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
        "-DLLVM_TARGETS_TO_BUILD=all",
        "-DLLVM_USE_LINKER=lld",
        "-DLLVM_DISABLE_ASSEMBLY_FILES=ON",
        # Distribution
        f"-DLLVM_DISTRIBUTION_COMPONENTS={components_joined}",
    ]

    ccache_env = os.environ.get("CCACHE_PROGRAM") or os.environ.get("CCACHE")
    ccache_program = shutil.which(ccache_env) if ccache_env else shutil.which("ccache")
    if not ccache_program and ccache_env:
        # Fall back to the env value as-is if it points to a real path.
        candidate = Path(ccache_env)
        if candidate.exists():
            ccache_program = candidate.as_posix()

    if ccache_program:
        ccache_path = Path(ccache_program).as_posix()
        print(f"Using ccache: {ccache_path}")
        cmake_args.append("-DLLVM_CCACHE_BUILD=ON")
        cmake_args.append(f"-DCCACHE_PROGRAM={ccache_path}")
    else:
        print("ccache not found; proceeding without it.")

    is_shared = "OFF"
    if args.mode == "Debug":
        cmake_args.append("-DCMAKE_BUILD_TYPE=Debug")
        cmake_args.append("-DLLVM_USE_SANITIZER=Address")
        is_shared = "ON"
    elif args.mode == "Release":
        cmake_args.append("-DCMAKE_BUILD_TYPE=Release")
    elif args.mode == "RelWithDebInfo":
        cmake_args.append("-DCMAKE_BUILD_TYPE=RelWithDebInfo")

    if sys.platform == "win32":
        is_shared = "OFF"
    cmake_args.append(f"-DBUILD_SHARED_LIBS={is_shared}")

    if lto_enabled:
        cmake_args.append("-DLLVM_ENABLE_LTO=Thin")
    else:
        cmake_args.append("-DLLVM_ENABLE_LTO=OFF")

    build_dir.mkdir(exist_ok=True)

    print(f"\nConfiguring in {build_dir}...")
    try:
        source_dir = project_root / "llvm"
        subprocess.check_call(
            ["cmake", "-S", str(source_dir), "-B", str(build_dir)] + cmake_args
        )
    except subprocess.CalledProcessError:
        print("CMake configuration failed!")
        sys.exit(1)

    print("\nBuilding 'install-distribution' target...")
    try:
        subprocess.check_call(
            ["cmake", "--build", str(build_dir), "--target", "install-distribution"]
        )
    except subprocess.CalledProcessError:
        print("Build failed!")
        sys.exit(1)

    print("\nCopying internal Sema headers...")
    clang_sema_dir = project_root / "clang/lib/Sema"
    install_sema_dir = install_prefix / "include/clang/Sema"
    install_sema_dir.mkdir(parents=True, exist_ok=True)

    headers_to_copy = ["CoroutineStmtBuilder.h", "TypeLocBuilder.h", "TreeTransform.h"]

    for header in headers_to_copy:
        src = clang_sema_dir / header
        dst = install_sema_dir / header
        if src.exists():
            shutil.copy(src, dst)
            print(f"  Copied {header}")
        else:
            print(f"  Warning: {header} not found in source.")

    def human_readable(num: int) -> str:
        for unit in ["B", "KB", "MB", "GB"]:
            if num < 1024.0:
                return f"{num:,.1f}{unit}"
            num /= 1024.0
        return f"{num:.1f}TB"

    lib_dir = install_prefix / "lib"
    sizes = []
    if lib_dir.exists():
        for p in lib_dir.rglob("*"):
            if p.is_file():
                sizes.append((p, p.stat().st_size))
    sizes.sort(key=lambda x: x[1], reverse=True)

    total_size = sum(sz for _, sz in sizes)
    print(f"\nLibrary size summary under {lib_dir}:")
    print(f"  Total: {human_readable(total_size)} across {len(sizes)} files")
    for path, sz in sizes:
        rel = path.relative_to(install_prefix)
        print(f"  {human_readable(sz):>8}  {rel}")
    if not sizes:
        print("  (no files found)")

    print(f"\nSuccess! Artifacts installed to: {install_prefix}")


if __name__ == "__main__":
    main()
