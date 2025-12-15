#!/usr/bin/env python3
import os
import subprocess
import shutil
import argparse
import sys


def get_build_config(build_type):
    """Return build configuration based on build type"""
    configs = {
        "debug": {
            "build_dir": "build-debug",
            "source_dir": ".",
            "install_dir": "./build-debug-install",
            "build_type": "Debug",
            "sanitizer": "Address",
            "shared_libs": True,
        },
        "thread": {
            "build_dir": "build-thread",
            "source_dir": ".",
            "install_dir": "./build-thread-install",
            "build_type": "Debug",
            "sanitizer": "Thread",
            "shared_libs": True,
        },
        "release": {
            "build_dir": "build-release",
            "source_dir": ".",
            "install_dir": "./build-release-install",
            "build_type": "Release",
            "sanitizer": None,
            "shared_libs": False,
        },
    }

    if build_type not in configs:
        print(f"Error: Unsupported build type '{build_type}'")
        print(f"Supported build types: {', '.join(configs.keys())}")
        sys.exit(1)

    return configs[build_type]


def build_llvm(config):
    """Execute LLVM build process"""
    print(f"Starting LLVM build ({config['build_type']} mode)")

    # Build CMake command
    config_args = [
        "cmake",
        "-G",
        "Ninja",
        "-S",
        "./llvm",
        "-B",
        config["build_dir"],
        "-DLLVM_USE_LINKER=lld",
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_BUILD_TYPE=" + config["build_type"],
        # Only build native to speed up building llvm.
        "-DLLVM_TARGETS_TO_BUILD=Native",
        "-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra",
        "-DCMAKE_INSTALL_PREFIX=" + config["install_dir"],
    ]

    # Add optional configurations

    # Build shared lib to reduce binary size and speed up linking
    if config["shared_libs"]:
        config_args.append("-DBUILD_SHARED_LIBS=ON")

    if config["sanitizer"]:
        config_args.append(f"-DLLVM_USE_SANITIZER={config['sanitizer']}")

    # CMake build command list
    cmake_commands = [
        config_args,
        ["cmake", "--build", config["build_dir"]],
        ["cmake", "--install", config["build_dir"]],
    ]

    # Execute CMake commands
    for i, command in enumerate(cmake_commands):
        print(f"Executing command {i + 1}/{len(cmake_commands)}: {' '.join(command)}")
        try:
            subprocess.run(command, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Build failed: {e}")
            sys.exit(1)


def copy_header_files(config):
    """Copy header files to installation directory"""
    print("Copying header files...")

    src = os.path.join(config["source_dir"], "./clang/lib/Sema/")
    dst = os.path.join(config["install_dir"], "include/clang/Sema/")
    files = ["CoroutineStmtBuilder.h", "TypeLocBuilder.h", "TreeTransform.h"]

    # Ensure target directory exists
    os.makedirs(dst, exist_ok=True)

    for file in files:
        src_file = os.path.join(src, file)
        dst_file = os.path.join(dst, file)

        if os.path.exists(src_file):
            print(f"Copying {src_file} to {dst_file}")
            shutil.copy(src_file, dst_file)
        else:
            print(f"Warning: Source file does not exist {src_file}")


def build():
    parser = argparse.ArgumentParser(
        description="Build LLVM libraries with different build configurations",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Build type descriptions:
  debug    - Debug mode with Address Sanitizer, builds shared libraries
  thread   - Debug mode with Thread Sanitizer, builds shared libraries
  release  - Release mode without sanitizer, builds static libraries

Examples:
  python build-llvm-libs.py build debug
  python build-llvm-libs.py build thread
  python build-llvm-libs.py build release
        """,
    )

    parser.add_argument(
        "build_type",
        choices=["debug", "thread", "release"],
        help="Build type (debug/thread/release)",
    )

    parser.add_argument(
        "--skip-copy", action="store_true", help="Skip header file copying step"
    )

    args = parser.parse_args(args=sys.argv[2:])

    # Get build configuration
    config = get_build_config(args.build_type)

    print("=== LLVM Build Configuration ===")
    print(f"Build type: {args.build_type}")
    print(f"Build directory: {config['build_dir']}")
    print(f"Install directory: {config['install_dir']}")
    print(f"CMake build type: {config['build_type']}")
    if config["sanitizer"]:
        print(f"Sanitizer: {config['sanitizer']}")
    print(f"Shared libraries: {config['shared_libs']}")
    print("=" * 30)

    # Execute build
    build_llvm(config)

    # Copy header files (unless skipped)
    if not args.skip_copy:
        copy_header_files(config)

    print(f"\n✅ LLVM {args.build_type} build completed!")
    print(f"Install directory: {config['install_dir']}")


def copy():
    parser = argparse.ArgumentParser(
        description="Copy header files to installation directory",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Build type descriptions:
  debug    - Debug mode with Address Sanitizer, builds shared libraries
  thread   - Debug mode with Thread Sanitizer, builds shared libraries
  release  - Release mode without sanitizer, builds static libraries
Optional arguments:
  --source_dir - Source directory
  --install_dir - Install directory

Examples:
  python build-llvm-libs.py copy debug
  python build-llvm-libs.py copy thread
  python build-llvm-libs.py copy release
        """,
    )
    parser.add_argument(
        "build_type",
        choices=["debug", "thread", "release"],
        help="Build type (debug/thread/release)",
    )
    parser.add_argument(
        "--source_dir",
        help="Source directory",
    )
    parser.add_argument(
        "--install_dir",
        help="Install directory",
    )
    args = parser.parse_args(args=sys.argv[2:])

    # Get build configuration
    config = get_build_config(args.build_type)
    if args.source_dir:
        config["source_dir"] = args.source_dir
    if args.install_dir:
        config["install_dir"] = args.install_dir

    copy_header_files(config)


def main():
    if len(sys.argv) < 2:
        print("Usage: python build-llvm-libs.py <command> [options]")
        print("Commands:")
        print("  build <build_type>")
        print(
            "  copy <build_type> [--source_dir <source_dir>] [--install_dir <install_dir>]"
        )
        sys.exit(1)
    command = sys.argv[1]
    if command == "build":
        build()
    elif command == "copy":
        copy()
    else:
        print(f"Unknown command: {command}")
        sys.exit(1)


if __name__ == "__main__":
    main()
