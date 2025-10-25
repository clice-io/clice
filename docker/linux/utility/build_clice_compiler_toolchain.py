# ========================================================================
# 🚀 Clice Compiler Toolchain Builder
# ========================================================================
# File: docker/linux/utility/build_clice_compiler_toolchain.py
# Purpose: Automated toolchain construction orchestrator
# 
# This module implements a high-performance parallel build system for
# constructing the complete Clice compiler toolchain from source.
# 
# Components Built:
#   • glibc (GNU C Library)
#   • GCC libstdc++ (C++ Standard Library) 
#   • Linux Kernel Headers
#   • LLVM Project (prepared for future builds)
# 
# Features:
#   • Parallel task execution with dependency resolution
#   • Robust error handling and recovery
#   • GPG signature verification
#   • Automated path fixing for relocatable builds
# ========================================================================

"""
🏗️ Clice Compiler Toolchain Builder

A sophisticated build orchestrator that constructs a complete compiler toolchain
from source components using parallel execution and dependency management.

This system builds the fundamental components required for the Clice development
environment, including system libraries, C++ standard library, and kernel headers.
All components are built with careful attention to compatibility and performance.

The build process is organized into clearly defined stages:
1. 📦 Setup - Install prerequisites and prepare environment
2. ⬇️ Download - Fetch source archives with verification
3. 📂 Extract - Unpack source code to build directories  
4. 🔨 Build - Compile and install components with proper configuration
5. 🔧 Post-process - Fix paths and finalize installation

Each stage is executed in parallel where possible, with automatic dependency
resolution ensuring correct build order.
"""

import sys
import os

# ========================================================================
# 🔧 Project Path Configuration
# ========================================================================
# Dynamic project root discovery - enables importing from parent directories
# This allows the utility scripts to access shared configuration modules
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

# ========================================================================
# 📚 Standard Library Imports
# ========================================================================
import shutil        # High-level file operations
import tarfile       # Archive extraction capabilities
from typing import Dict, Set  # Type hints for better code clarity

# ========================================================================
# 🛠️ Build System Components
# ========================================================================
from build_utils import (
    Job,                    # Individual build task representation
    ParallelTaskScheduler,  # High-performance parallel execution engine
    download_file,          # Accelerated file download with aria2c
    run_command,           # Shell command execution with environment control
    verify_signature,      # GPG signature verification
    # Generic component build utilities
    install_download_prerequisites,  # Download prerequisite installation
    install_extract_prerequisites,   # Extract prerequisite installation
    download_and_verify,            # Component source download and verification
    extract_source,                 # Component source extraction
)

# ========================================================================
# ⚙️ Configuration Constants
# ========================================================================
from config.build_config import (
    TOOLCHAIN_BUILD_ROOT,              # Build root directory
    GPG_KEY_SERVER,                    # GPG keyserver list
    TOOLCHAIN_BUILD_ENV_VARS,          # Build environment variables
    # Import component instances for structured access
    TOOLCHAIN
)

# ========================================================================
# 🎯 Build Task Implementations
# ========================================================================
# Each function represents a discrete build task that can be executed
# independently once its dependencies are satisfied. The parallel scheduler
# coordinates execution order based on the dependency graph.
# ========================================================================

# ========================================================================
# 📦 Environment Setup Tasks
# ========================================================================

def update_apt():
    """
    🔄 Update APT Package Database
    
    Refreshes the APT package manager's local database to ensure we have
    access to the latest package versions and security updates.
    
    This is the foundation step that must complete before any package 
    installation can proceed safely.
    """
    print("🔄 [SETUP] Refreshing APT package database...")
    run_command("apt update")



def install_build_prerequisites():
    """
    🔨 Install Build Stage Prerequisites
    
    Installs the complete build environment including:
    • Core build tools (make, binutils, rsync)
    • Text processing tools (gawk, bison) for glibc
    • GCC 9 toolchain for glibc compilation
    • GCC 14 toolchain for libstdc++ compilation
    
    Note: We maintain multiple GCC versions because glibc requires
    GCC < 10 to avoid linker symbol conflicts, while modern libstdc++
    benefits from the latest compiler features.
    """
    print("🔨 [SETUP] Installing comprehensive build environment...")
    print("    📋 Components: make, binutils, gawk, bison, gcc-9, gcc-14")
    build_prerequisites = TOOLCHAIN.build_prerequisites
    pkg_list = " ".join(build_prerequisites)
    run_command(f"apt install -y --no-install-recommends -o APT::Keep-Downloaded-Packages=true {pkg_list}")
    # linux headers install requires gcc, even though we won't use it in linux header install
    run_command("update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 90")
    print("✅ [SETUP] Build environment ready")



# ========================================================================
# 📚 GNU C Library (glibc) Tasks
# ========================================================================
# glibc is the core system library providing POSIX API implementation.
# It requires special handling due to its fundamental role in the system.
# ========================================================================

def fix_glibc_paths():
    """
    🔧 Fix Hardcoded Build Paths in glibc Installation
    
    glibc's build process generates various text files (.la, .pc, linker scripts)
    that contain hardcoded absolute paths from the build environment. These paths
    need to be cleaned up to create relocatable installations.
    
    This function scans all installed files and removes build-specific paths,
    making the toolchain portable across different installation directories.
    
    Process:
    1. Walk through all installed files
    2. Identify text files (skip binaries)
    3. Search for hardcoded paths
    4. Remove absolute path references
    5. Preserve relative path structure
    """
    search_path = TOOLCHAIN.sysroot_dir
    print(f"🔧 [POST-PROCESS] Sanitizing hardcoded paths in {search_path}...")

    if not os.path.isdir(search_path):
        print(f"❌ [ERROR] Sysroot directory not found: '{search_path}'", file=sys.stderr)
        return

    files_processed = 0
    for root, _, files in os.walk(search_path):
        for filename in files:
            file_path = os.path.join(root, filename)
            
            # Check if file is text-based (skip binaries)
            try:
                with open(file_path, 'r', encoding='utf-8') as f:
                    original_content = f.read()
                    if '\0' in original_content:  # Contains null bytes (binary)
                        continue
            except UnicodeDecodeError:
                continue  # File not readable or not text

            # Look for and remove hardcoded paths
            replacement_path = f"{os.path.dirname(file_path)}/"            
            new_content = original_content.replace(replacement_path, "")
            if new_content == original_content:
                continue  # No changes needed

            # Apply the path fix
            print(f"    🔨 Fixing paths in: {os.path.relpath(file_path, search_path)}")
            print(f"      ➤ Removing: '{replacement_path}'")
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(new_content)
            files_processed += 1
    
    print(f"✅ [POST-PROCESS] Path fixing complete ({files_processed} files processed)")


def build_and_install_glibc(component):
    """
    🏗️ Build and Install GNU C Library (glibc)
    
    Configures, compiles, and installs glibc - the foundational C library
    that provides POSIX API implementation and system call interface.
    
    Build Configuration:
    • Uses GCC 9 (required: GCC < 10 to avoid symbol conflicts)
    • Targets x86_64 architecture with 64-bit support
    • Disables compiler warnings as errors (--disable-werror)
    • Enables 64-bit libraries, disables 32-bit compatibility
    
    Post-installation includes path sanitization to ensure relocatable builds.
    
    Note: glibc is built out-of-tree in a separate build directory to
    maintain clean separation between source and build artifacts.
    """
    print(f"🏗️ [BUILD] Starting {component.name} compilation...")
    print(f"    📋 Using GCC 9 (required for glibc compatibility)")
    print(f"    🎯 Target: {TOOLCHAIN.host_triplet} ({TOOLCHAIN.host_machine})")
    print(f"    📁 Install: {TOOLCHAIN.sysroot_dir}/usr")
    
    # Prepare out-of-tree build directory
    os.makedirs(component.build_dir, exist_ok=True)
    
    # Configure build environment with GCC 9
    compiler_env = {
        'CC': 'gcc-9',       # GNU C compiler version 9 (full path)
        'CXX': 'g++-9',      # GNU C++ compiler version 9 (full path)
        'CPP': 'cpp-9',      # C preprocessor (explicit)
    }
    compiler_env.update(TOOLCHAIN_BUILD_ENV_VARS)

    # Configure glibc build
    print(f"⚙️ [CONFIG] Configuring glibc build...")
    configure_script = os.path.join(component.src_dir, "configure")
    configure_command = f"{configure_script} --host={TOOLCHAIN.host_triplet} --prefix={TOOLCHAIN.sysroot_dir}/usr --disable-werror --disable-lib32 --enable-lib64"
    run_command(configure_command, cwd=component.build_dir, env=compiler_env)
    
    # Compile glibc
    print(f"🔨 [COMPILE] Building glibc (this may take several minutes)...")
    run_command("make -j", cwd=component.build_dir, env=compiler_env)
    
    # Install glibc to sysroot
    print(f"📦 [INSTALL] Installing glibc to sysroot...")
    run_command(f"make install -j", cwd=component.build_dir, env=compiler_env)

    # Post-process to fix hardcoded paths
    fix_glibc_paths()
    print(f"✅ [COMPLETE] glibc build and installation finished")

# ========================================================================
# 🐧 Linux Kernel Headers Installation
# ========================================================================
# Kernel headers provide system call definitions and kernel API interfaces
# required for userspace programs to interact with the Linux kernel.
# ========================================================================

def install_linux_headers(component):
    """
    🐧 Install Linux Kernel Headers
    
    Extracts and installs sanitized Linux kernel headers that provide
    system call definitions and kernel API interfaces for userspace programs.
    
    The kernel headers are essential for:
    • System call interface definitions
    • Kernel data structure layouts
    • Device driver interfaces
    • Architecture-specific constants
    
    Installation Process:
    1. Use kernel's built-in header installation system
    2. Filter out kernel-internal definitions
    3. Install sanitized headers to sysroot/usr
    4. Ensure compatibility with userspace programs
    """
    install_path = os.path.join(TOOLCHAIN.sysroot_dir, "usr")
    print(f"🐧 [INSTALL] Installing Linux kernel headers...")
    print(f"    🏗️ Architecture: {TOOLCHAIN.host_machine}")
    print(f"    📁 Target: {install_path}")
    
    # Use command-line arguments instead of environment variables
    # This ensures highest priority and avoids Makefile variable conflicts
    # Install to /usr within sysroot for Clang compatibility
    make_args: Dict[str, str] = {
        "ARCH": TOOLCHAIN.host_machine,
        "INSTALL_HDR_PATH": install_path
    }
    
    # Also preserve any global build environment variables
    make_env = {}
    make_env.update(TOOLCHAIN_BUILD_ENV_VARS)

    # Build the make command with arguments
    args_str = " ".join([f"{key}={value}" for key, value in make_args.items()])
    
    # Install sanitized kernel headers using command-line parameters
    run_command(f"make {args_str} -j headers_install", cwd=component.src_dir, env=make_env)
    print(f"✅ [COMPLETE] Linux kernel headers installed")

# ========================================================================
# 🛠️ GCC Compiler Collection Tasks
# ========================================================================
# GCC provides the C++ standard library (libstdc++) and essential runtime
# libraries. We build only the target libraries, not the full compiler.
# ========================================================================

def download_gcc_prerequisites(component):
    """
    📦 Download GCC Mathematical Prerequisites
    
    Downloads and sets up the mathematical libraries required for GCC:
    • GMP (GNU Multiple Precision Arithmetic Library)
    • MPFR (Multiple Precision Floating-Point Reliable Library)  
    • MPC (Multiple Precision Complex Library)
    
    These libraries are essential for GCC's internal computations and
    optimizations. The GCC source tree includes a convenience script
    that automatically downloads the correct versions.
    """
    print(f"📦 [DOWNLOAD] Fetching {component.name} mathematical prerequisites...")
    print(f"    📋 Components: GMP, MPFR, MPC")
    run_command("./contrib/download_prerequisites", cwd=component.src_dir)
    print(f"✅ [DOWNLOAD] GCC prerequisites ready")

def build_and_install_libstdcpp(component):
    """
    🏗️ Build and Install C++ Standard Library (libstdc++)
    
    Builds the C++ standard library and essential runtime libraries from GCC.
    We configure GCC but only build the target libraries we need, avoiding
    the full compiler build which would be unnecessary and time-consuming.
    
    Target Libraries Built:
    • libgcc - Low-level runtime support (exception handling, etc.)
    • libstdc++-v3 - Complete C++ standard library
    • libsanitizer - Address/memory/thread sanitizer support
    • libatomic - Atomic operations for lock-free programming
    • libbacktrace - Stack backtrace support for debugging
    • libgomp - OpenMP parallel programming runtime
    • libquadmath - Quadruple precision floating-point math
    
    Configuration highlights:
    • Uses modern GCC 14 for latest C++ features
    • Links against our custom glibc build
    • Enables LTO for better optimization
    • Static linking for portable distribution
    """
    print(f"🏗️ [BUILD] Starting {component.name} C++ standard library build...")
    print(f"    📋 Using GCC 14 (modern C++ support)")
    print(f"    🎯 Target libraries: {', '.join(component.target_libs)}")
    print(f"    🔗 Linking with glibc v{TOOLCHAIN.glibc.version}")
    
    # Prepare out-of-tree build directory
    os.makedirs(component.build_dir, exist_ok=True)

    # Configure build environment with modern GCC
    compiler_env = {
        'CC': 'gcc-14',          # Modern C compiler (full path)
        'CXX': 'g++-14',         # Modern C++ compiler (full path)
        'CPP': 'cpp-14',         # C preprocessor (explicit)
    }
    compiler_env.update(TOOLCHAIN_BUILD_ENV_VARS)

    # Configure GCC for target library building
    print(f"⚙️ [CONFIG] Configuring GCC for library-only build...")
    configure_cmd = [
        f"{component.src_dir}/configure",
        f"--host={TOOLCHAIN.host_triplet}",                    # Build system
        f"--target={TOOLCHAIN.target_triplet}",                # Target system
        f"--prefix={TOOLCHAIN.sysroot_dir}/usr",               # Installation prefix
        f"--with-sysroot={TOOLCHAIN.sysroot_dir}",             # System root for headers/libs
        f"--with-glibc-version={TOOLCHAIN.glibc.version}",     # glibc compatibility
        "--with-gcc-major-version-only",                       # Use major version in paths for clang compatibility
        "--disable-werror",                                    # Don't fail on warnings
        "--disable-multilib",                                  # Single architecture only
        "--disable-bootstrap",                                 # Skip multi-stage build
        "--enable-languages=c,c++",                            # Language support
        "--enable-threads",                                    # Threading support
        "--enable-lto",                                        # Link-time optimization
        "--enable-nls",                                        # Native language support
        "--disable-shared",                                    # Static libraries for portability
    ]
    run_command(" ".join(configure_cmd), cwd=component.build_dir, env=compiler_env)
    
    # Build only the target libraries we need
    print(f"🔨 [COMPILE] Building target libraries (this will take significant time)...")
    build_targets = " ".join([f"all-target-{lib}" for lib in component.target_libs])
    run_command(f"make -j {build_targets}", cwd=component.build_dir, env=compiler_env)
    
    # Install the built libraries
    print(f"📦 [INSTALL] Installing C++ standard library and runtime libraries...")
    install_targets = " ".join([f"install-target-{lib}" for lib in component.target_libs])
    run_command(f"make -j {install_targets}", cwd=component.build_dir, env=compiler_env)
    print(f"✅ [COMPLETE] C++ standard library build finished")

# ========================================================================
# 🎭 Main Build Orchestrator
# ========================================================================

def main():
    """
    🚀 Main Toolchain Build Orchestrator
    
    Coordinates the entire toolchain build process using a sophisticated
    parallel task scheduler with dependency resolution. The build is organized
    as a directed acyclic graph (DAG) where each node represents a build task
    and edges represent dependencies.
    
    Build Phases:
    1. 🔄 Setup - System preparation and prerequisite installation
    2. ⬇️ Download - Source code fetching with verification
    3. 📂 Extract - Archive extraction and preparation
    4. 🏗️ Build - Compilation and installation
    
    The scheduler automatically determines the optimal execution order and
    runs independent tasks in parallel to minimize total build time.
    
    Dependency Graph Structure:
    • Setup tasks run first and can execute in parallel
    • Download tasks depend on download prerequisites
    • Extract tasks depend on both download completion and extract tools
    • Build tasks have complex interdependencies (glibc before libstdc++)
    """
    print("🚀 ========================================================================")
    print("🚀 CLICE COMPILER TOOLCHAIN BUILD SYSTEM")
    print("🚀 ========================================================================")
    print(f"📁 Sysroot Directory: {TOOLCHAIN.sysroot_dir}")
    print(f"🎯 Target Architecture: {TOOLCHAIN.target_triplet} ({TOOLCHAIN.target_machine})")
    print(f"📋 Components: glibc, Linux headers, libstdc++, LLVM (prepared)")
    print("🚀 ========================================================================\n")

    # ====================================================================
    # 📋 Build Task Registry
    # ====================================================================
    # Each job represents an atomic build operation that can be executed
    # independently once its dependencies are satisfied.
    # ====================================================================
    
    all_jobs: Dict[str, Job] = {
        # 📦 System Setup Tasks
        "update_apt": Job("update_apt", update_apt),
        "install_download_prerequisites": Job("install_download_prerequisites", install_download_prerequisites),
        "install_extract_prerequisites": Job("install_extract_prerequisites", install_extract_prerequisites),
        "install_build_prerequisites": Job("install_build_prerequisites", install_build_prerequisites),

        # 📚 GNU C Library (glibc) Pipeline
        "download_glibc": Job("download_glibc", download_and_verify, (TOOLCHAIN.glibc,)),
        "extract_glibc": Job("extract_glibc", extract_source, (TOOLCHAIN.glibc,)),
        "build_and_install_glibc": Job("build_and_install_glibc", build_and_install_glibc, (TOOLCHAIN.glibc,)),
        
        # 🐧 Linux Kernel Headers Pipeline  
        "download_linux": Job("download_linux", download_and_verify, (TOOLCHAIN.linux,)),
        "extract_linux": Job("extract_linux", extract_source, (TOOLCHAIN.linux,)),
        "install_linux_headers": Job("install_linux_headers", install_linux_headers, (TOOLCHAIN.linux,)),

        # 🛠️ GCC C++ Standard Library Pipeline
        "download_gcc": Job("download_gcc", download_and_verify, (TOOLCHAIN.gcc,)),
        "extract_gcc": Job("extract_gcc", extract_source, (TOOLCHAIN.gcc,)),
        "download_gcc_prerequisites": Job("download_gcc_prerequisites", download_gcc_prerequisites, (TOOLCHAIN.gcc,)),
        "build_and_install_libstdcpp": Job("build_and_install_libstdcpp", build_and_install_libstdcpp, (TOOLCHAIN.gcc,)),

        # ⚡ LLVM Project Pipeline (prepared for future builds)
        "download_llvm": Job("download_llvm", download_and_verify, (TOOLCHAIN.llvm,)),
        "extract_llvm": Job("extract_llvm", extract_source, (TOOLCHAIN.llvm,)),
    }

    # ====================================================================
    # 🔗 Dependency Graph Definition
    # ====================================================================
    # Defines the build order constraints. Each task lists its prerequisites
    # that must complete before it can begin execution.
    # ====================================================================
    
    dependency_graph: Dict[str, Set[str]] = {
        # 📦 Setup Phase - Foundation tasks
        "update_apt": set(),  # No dependencies - can start immediately
        "install_download_prerequisites": {"update_apt"},
        "install_extract_prerequisites": {"update_apt"},
        "install_build_prerequisites": {"update_apt"},
        
        # 📚 glibc Build Pipeline
        "download_glibc": {"install_download_prerequisites"},
        "extract_glibc": {"download_glibc", "install_extract_prerequisites"},
        "build_and_install_glibc": {"extract_glibc", "install_build_prerequisites"},
        
        # 🐧 Linux Headers Pipeline (can run parallel with glibc download/extract)
        "download_linux": {"install_download_prerequisites"},
        "extract_linux": {"download_linux", "install_extract_prerequisites"},
        "install_linux_headers": {"extract_linux", "install_build_prerequisites"},

        # 🛠️ GCC Pipeline (requires glibc and kernel headers)
        "download_gcc": {"install_download_prerequisites"},
        "extract_gcc": {"download_gcc", "install_extract_prerequisites"},
        "download_gcc_prerequisites": {"extract_gcc"},
        "build_and_install_libstdcpp": {
            "download_gcc_prerequisites",  # GCC math libraries ready
            "build_and_install_glibc",     # System library available
            "install_linux_headers",       # Kernel interfaces available
            "install_build_prerequisites"  # Build tools ready
        },

        # ⚡ LLVM Pipeline (prepared for future expansion)
        "download_llvm": {"install_download_prerequisites"},
        "extract_llvm": {"download_llvm", "install_extract_prerequisites"}
    }

    # ====================================================================
    # 🚀 Launch Parallel Build System
    # ====================================================================
    print(f"📊 Initializing parallel scheduler with {len(all_jobs)} tasks...")
    print(f"🔗 Total dependencies: {sum(len(deps) for deps in dependency_graph.values())}")
    print(f"⚡ Maximum parallelism: {len([job for job, deps in dependency_graph.items() if not deps])} initial tasks\n")
    
    scheduler = ParallelTaskScheduler(all_jobs, dependency_graph)
    scheduler.run()

    print("\n🎉 ========================================================================")
    print("🎉 TOOLCHAIN BUILD COMPLETED SUCCESSFULLY!")
    print("🎉 ========================================================================")
    print(f"✅ All components built and installed to: {TOOLCHAIN.sysroot_dir}")
    print("🎉 ========================================================================")

if __name__ == "__main__":
    main()

# Here's origin toolchain build bash, won't be updated, just for reference
# the only target is to build static link libstdc++, without full parallel build support

# prerequests
"""
# aria2 is used for downloading files
# gawk bison are for glibc build
# bzip2 is for extracting tar.bz2 files when prepare gcc prerequisites
# rsync is required by linux kernel headers installation
apt install -y --no-install-recommends aria2 bzip2 rsync gawk bison
# gcc-9 for glibc build
# gcc-14 for libstdc++ build
apt install -y --no-install-recommends binutils gcc-9 libstdc++-9-dev gcc-14 g++-14 libstdc++-9-dev
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 90
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 90
update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-14 90
update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-14 90
update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 80
update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-9 80
"""

# generic command
"""
export ORIGIN='$$ORIGIN' # to generate rpath relative to the binary location
export HOST=x86_64-linux-gnu # this is not essential, could be moved to python config
export TARGET=${HOST} # this is not essential, could be moved to python config
export PREFIX="${TOOLCHAIN_BUILD_ROOT}/sysroot/${HOST}/${TARGET}/glibc${versions["glibc"]}-libstdc++${versions["gcc"]}-linux${versions["linux"]}" # this is not essential, could be moved to python config
export ARCH=x86_64 # this is not essential, could be moved to python config
"""

# build glibc
"""
# Attention: gcc version less than 10 required, or multiple definition __libc_use_alloca would be error
update-alternatives --set gcc /usr/bin/gcc-9
update-alternatives --set cc /usr/bin/gcc-9

mkdir -p GLIBC_CONFIG.build_dir
cd GLIBC_CONFIG.build_dir

../configure --host=${HOST} --prefix=${PREFIX}/usr --disable-werror --disable-lib32 --enable-lib64
make -j
make install -j

# This script is intended to be run after the glibc build process.
# Its purpose is to find and replace placeholder paths within the generated text-based
# files (like .la, .pc, etc.) located under the ${PREFIX} directory.
# This is a common post-build step to fix hardcoded paths from the build environment.

# --- Configuration ---

# 1. The root directory to search within. It's expected that the
#    PREFIX environment variable is set by the build environment.
SEARCH_PATH="${PREFIX}"

# --- Script Body ---

# Check if the search path is valid
if [ -z "$SEARCH_PATH" ] || [ ! -d "$SEARCH_PATH" ]; then
    echo "Error: SEARCH_PATH is not set or is not a valid directory: '$SEARCH_PATH'"
    exit 1
fi

# Check if the 'file' command is available
if ! command -v file &> /dev/null; then
    echo "Error: 'file' command not found. Please install it to proceed."
    exit 1
fi

echo "Removing absolute paths from text ld scripts..."
echo "Starting search in: '$SEARCH_PATH'"
echo "========================================"

# Find all files, then check each one to see if it's a text file containing the search string.
# Using -print0 and read -d '' handles filenames with spaces or special characters.
find "$SEARCH_PATH" -type f -print0 | while IFS= read -r -d '' file; do
    
    # Check if the file is a text file
    MIME_TYPE=$(file -b --mime-type "$file")
    if [[ "$MIME_TYPE" != text/* ]]; then
        echo "--- Skipping binary file: $file (Type: $MIME_TYPE) ---"
        continue
    fi

    # Get the directory where the file is located.
    REPLACEMENT_PATH=$(dirname "$file")
    REPLACEMENT_PATH="${REPLACEMENT_PATH}/"
    
    # Check if the file actually contains the search string before processing
    if ! grep -q "$REPLACEMENT_PATH" "$file"; then
        continue
    fi

    # It's a text file and contains the string, so process it.
    echo -e "\n--- Processing text file: $file ---"



    # Use grep to show where the changes will happen.
    echo "  Matches found on lines:"
    grep -n "$REPLACEMENT_PATH" "$file" | sed 's/^/    /g'
    
    echo "  Deleting '$REPLACEMENT_PATH'"

    # Perform the replacement in-place using sed.
    # The delimiter `|` is used to avoid conflicts if paths contain `/`.
    sed -i "s|$REPLACEMENT_PATH||g" "$file"

done

echo "========================================"
echo "Path replacement process finished."
"""

# build linux kernel headers(parallel with glibc build)
"""
export LINUX_SRC_URL="https://github.com/torvalds/linux/archive/refs/tags/v${versions["linux"]}.zip"
git clone https://github.com/torvalds/linux.git --depth=1 LINUX_CONFIG.src_dir # should replace with download and extract using LINUX_SRC_URL
cd LINUX_CONFIG.src_dir
make ARCH=x86_64 INSTALL_HDR_PATH=${PREFIX}/usr -j headers_install
"""

# build libstdc++(requires glibc built and kernel headers installed)
"""
# Download prerequisites for GCC
cd GCC_CONFIG.src_dir
contrib/download_prerequisites

# build libstdc++
# libstdc++ could not be built separately, so we build the whole GCC but only install libstdc++
update-alternatives --set gcc /usr/bin/gcc-14
update-alternatives --set g++ /usr/bin/g++-14
update-alternatives --set cc /usr/bin/gcc-14
update-alternatives --set c++ /usr/bin/g++-14

mkdir -p GCC_CONFIG.build_dir
cd GCC_CONFIG.build_dir

../configure \
    --host=${TARGET} \
    --target=${TARGET} \
    --prefix=${PREFIX}/usr \
    --with-sysroot=${PREFIX} \
    --with-glibc-version=${versions["glibc"]} \
    --disable-werror \
    --disable-multilib \
    --disable-shared \
    --disable-bootstrap \
    --enable-languages=c,c++ \
    --enable-threads \
    --enable-lto \
    --enable-nls

make -j all-target-libgcc all-target-libstdc++-v3 all-target-libsanitizer all-target-libatomic all-target-libbacktrace all-target-libgomp all-target-libquadmath
make -j install-target-libgcc install-target-libstdc++-v3 install-target-libsanitizer install-target-libatomic install-target-libbacktrace install-target-libgomp install-target-libquadmath
"""
