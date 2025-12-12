#!/usr/bin/env python3
import sys
import subprocess
import shutil
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(
        description="Build LLVM with specific configurations."
    )
    parser.add_argument(
        "mode",
        nargs="?",
        default="release",
        choices=["debug", "release", "releasedbg"],
        help="Build mode (default: release)",
    )
    parser.add_argument(
        "lto",
        nargs="?",
        default="off",
        choices=["on", "off", "true", "false"],
        help="Enable LTO (default: off)",
    )

    args = parser.parse_args()

    project_root = Path.cwd()

    if not (project_root / "llvm" / "CMakeLists.txt").exists():
        print(f"Error: Could not find 'llvm/CMakeLists.txt' in {project_root}")
        print("Please run this script from the root of the llvm-project repository.")
        sys.exit(1)

    build_dir_name = f"build-{args.mode}"
    build_dir = project_root / build_dir_name
    install_prefix = project_root / f"{build_dir_name}-install"

    print("--- Configuration ---")
    print(f"Mode:           {args.mode}")
    print(f"LTO:            {args.lto}")
    print(f"Root:           {project_root}")
    print(f"Build Dir:      {build_dir}")
    print(f"Install Prefix: {install_prefix}")
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
        "clangStaticAnalyzerCore",
        "clangStaticAnalyzerFrontend",
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
        # Distribution
        f"-DLLVM_DISTRIBUTION_COMPONENTS={components_joined}",
    ]

    if sys.platform == "win32":
        cmake_args.append("-DCMAKE_C_COMPILER=clang-cl")
        cmake_args.append("-DCMAKE_CXX_COMPILER=clang-cl")
        cmake_args.append("-DLLVM_USE_LINKER=lld")
    else:
        cmake_args.append("-DCMAKE_C_COMPILER=clang")
        cmake_args.append("-DCMAKE_CXX_COMPILER=clang++")
        cmake_args.append("-DLLVM_USE_LINKER=lld")

        cmake_args.append("-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld")
        cmake_args.append("-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld")
        cmake_args.append("-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld")

    is_shared = "OFF"

    if args.mode == "debug":
        cmake_args.append("-DCMAKE_BUILD_TYPE=Debug")
        cmake_args.append("-DLLVM_USE_SANITIZER=Address")
        is_shared = "ON"
    elif args.mode == "release":
        cmake_args.append("-DCMAKE_BUILD_TYPE=Release")
    elif args.mode == "releasedbg":
        cmake_args.append("-DCMAKE_BUILD_TYPE=RelWithDebInfo")

    cmake_args.append(f"-DBUILD_SHARED_LIBS={is_shared}")

    if is_shared == "OFF":
        cmake_args.append("-DCLANG_BUILD_STATIC=ON")

    if args.lto.lower() in ["on", "true"]:
        cmake_args.append("-DLLVM_ENABLE_LTO=ON")
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
