# ========================================================================
# 🔧 Clice Toolchain Build Configuration  
# ========================================================================
# File: config/build_config.py
# Purpose: Centralized configuration constants for the toolchain build system
# 
# This module provides a comprehensive configuration framework for building
# the complete Clice compiler toolchain, including glibc, GCC, LLVM, and
# Linux kernel headers.
# ========================================================================

"""
🚀 Clice Toolchain Build Configuration

This module serves as the central configuration hub for the Clice toolchain
build process. It defines all necessary constants, paths, and component
configurations required to build a complete development environment.

Key Features:
    • Centralized toolchain component definitions
    • Environment variable management
    • Build dependency specifications
    • Cross-platform compatibility settings
    • Automated version management

Supported Components:
    • glibc (GNU C Library)
    • GCC (GNU Compiler Collection)
    • LLVM (Low Level Virtual Machine)
    • Linux Kernel Headers
    • CMake Build System
    • XMake Build System
"""

import json
import os
from typing import Any, List, Dict

# ========================================================================
# 🌍 Environment Variables and Core Paths
# ========================================================================

# Global environment variables that will be written to /root/.bashrc
# and utilized by the run_command execution framework
DEVELOPMENT_SHELL_VARS: Dict[str, str] = {
    "PATH": "/root/.local/bin:${PATH}",
    "XMAKE_ROOT": "y"  # Enable XMake auto-updating and self-management system
}

# Specialized environment variables for toolchain build processes
# These variables control linking behavior and runtime path resolution
TOOLCHAIN_BUILD_ENV_VARS: Dict[str, str] = {
    "ORIGIN": "$ORIGIN"  # Enable relative rpath for portable binary distribution
}

# Core project structure definitions
# CLICE_WORKDIR can be customized via environment variable, defaults to /clice
CLICE_WORKDIR: str = os.getenv("CLICE_WORKDIR", "")                                  # Working directory inside Docker container
PROJECT_ROOT: str = CLICE_WORKDIR                                                     # Root directory of the Clice project
PYPROJECT_PATH: str = os.path.join(PROJECT_ROOT, "pyproject.toml")             # Python project configuration file
TOOLCHAIN_BUILD_ROOT: str = "/toolchain-build"                                 # Root directory for all toolchain builds
TOOLCHAIN_CONFIG_PATH: str = os.path.join(PROJECT_ROOT, "config/default-toolchain-version.json")  # Version definitions

# ========================================================================
# 📦 Release Package Configuration (Cross-Stage Variables)
# ========================================================================

# These variables are designed to be passed across Docker build stages
# RELEASE_PACKAGE_DIR is the main package directory, PACKED_RELEASE_PACKAGE_PATH is the compressed archive
RELEASE_PACKAGE_DIR: str = os.getenv("RELEASE_PACKAGE_DIR", "")
PACKED_RELEASE_PACKAGE_PATH: str = os.getenv("PACKED_RELEASE_PACKAGE_PATH", "")
ENVIRONMENT_CONFIG_FILE: str = os.getenv("ENVIRONMENT_CONFIG_FILE", "")

# Source code cache directory for toolchain build
BUILD_CACHE_DIR: str = os.getenv("BUILD_CACHE_DIR", "")

WORKDIR_ROOT: str = "/dev-container-build"  # Temporary work directory for builds (not persistent)

# ========================================================================
# 🏗️ Dynamic Configuration Loading
# ========================================================================

# Dynamic version loading from configuration file
# This enables centralized version management across all build components
TOOLCHAIN_VERSIONS: Dict[str, Any] = {}
with open(TOOLCHAIN_CONFIG_PATH, "r") as f:
    TOOLCHAIN_VERSIONS = json.load(f)

# ========================================================================
# 🔐 GPG Verification Configuration
# ========================================================================

# Trusted GPG key servers for source code verification
# Multiple servers provide redundancy for package signature validation
GPG_KEY_SERVER: List[str] = [
    "keys.openpgp.org",      # Primary OpenPGP keyserver
    "keyserver.ubuntu.com"   # Ubuntu's reliable keyserver mirror
]

# ========================================================================
# 🧩 Component Architecture Definitions
# ========================================================================

class Component:
    """
    🔧 Base Component Configuration Class
    
    Provides a unified interface for all build components including APT packages,
    development tools, and toolchain components. Each component manages its own
    directory structure and download configuration.
    
    Attributes:
        name: Component identifier (unique across all components)
        version: Component version from TOOLCHAIN_VERSIONS (None for versionless components)
        package_dir: Directory for final packaged files in RELEASE_PACKAGE_DIR
        cache_dir: Directory for persistent caches (downloads, APT packages, etc.)
        work_dir: Directory for temporary build files (not persistent)
        base_url: Base URL pattern for downloads (class-level, optional)
        tarball_name_pattern: Tarball filename pattern (class-level, optional)
        verification_name_pattern: Verification filename pattern (class-level, optional)
        download_prerequisites: Tools required for downloading source code (class-level)
        extract_prerequisites: Tools required for extracting archives (class-level)
    """
    
    # Class-level URL patterns (overridden by subclasses)
    base_url: str = ""
    tarball_name_pattern: str = ""
    verification_name_pattern: str = ""
    
    # Class-level prerequisites configuration (overridden by subclasses)
    download_prerequisites: List[str] = [
        "aria2",                       # High-speed multi-connection downloader
        "gnupg",                       # GPG signature verification system
        "git",                         # Required for git clone llvm
    ]
    
    extract_prerequisites: List[str] = [
        "bzip2",                       # Required for .tar.bz2 archives (GCC prerequisites)
        "xz-utils",                    # Required for extracting .xz archives (toolchain sources)
    ]

    build_prerequisites: List[str] = []  # To be defined by subclasses if needed

    # Where the component will be deployed
    host_system: str = "linux"
    host_machine: str = "x86_64"

    # Where the constructed output (like clice binary) runs on
    target_system: str = host_system
    target_machine: str = host_machine

    def __init__(self, name: str, version: str = "unknown"):
        self.name = name
        self.version = version
        
        # Directory structure generation based on name and version
        self.package_dir = os.path.join(RELEASE_PACKAGE_DIR, self.versioned_name)
        self.cache_dir = os.path.join(BUILD_CACHE_DIR, self.versioned_name)  
        self.work_dir = os.path.join(WORKDIR_ROOT, self.versioned_name)

    @property
    def versioned_name(self) -> str:
        """Generate folder name from component name and version."""
        return f"{self.name}-{self.version}"

    @property
    def tarball_name(self) -> str:
        """Complete tarball filename."""
        if not self.tarball_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required tarball_name_pattern")

        return self.tarball_name_pattern.format(version=self.version, system=self.host_system, machine=self.host_machine)
        
    @property
    def tarball_url(self) -> str:
        """Complete download URL (requires base_url and tarball_name_pattern)."""
        if not self.base_url:
            raise ValueError(f"Component '{self.name}' missing required base_url")
        if not self.tarball_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required tarball_name_pattern")

        formatted_base_url = self.base_url.format(version=self.version, system=self.host_system, machine=self.host_machine)
        return f"{formatted_base_url}/{self.tarball_name}"

    @property
    def verification_name(self) -> str:
        """Complete verification filename."""
        if not self.verification_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required verification_name_pattern")

        return self.verification_name_pattern.format(version=self.version, system=self.host_system, machine=self.host_machine)

    @property
    def verification_url(self) -> str:
        """Verification file download URL."""
        formatted_base_url = self.base_url.format(version=self.version, system=self.host_system, machine=self.host_machine)
        return f"{formatted_base_url}/{self.verification_name}"

    @property
    def host_triplet(self) -> str:
        """Host system triplet (e.g., x86_64-linux-gnu)."""
        return f"{self.host_machine}-{self.host_system}-gnu"
    
    @property
    def target_triplet(self) -> str:
        """Target system triplet (e.g., x86_64-linux-gnu)."""
        return f"{self.target_machine}-{self.target_system}-gnu"


class ToolchainSubComponent(Component):
    """
    🔧 Toolchain Sub-Component Configuration Class
    
    Specialized component for toolchain elements (glibc, gcc, llvm, linux).
    Creates subdirectories under the main toolchain component structure.
    
    Additional Attributes:
        parent_component: Reference to parent toolchain component
        extracted_dir: Directory for extracted source codwe
        src_dir: Version-specific source directory  
        build_dir: Out-of-tree build directory
    """
    
    def __init__(self, name: str, parent_component: Component):
        version = TOOLCHAIN_VERSIONS[name]
        super().__init__(name, version)
        self.parent_component = parent_component
        
        # Override directory structure to be under parent toolchain
        self.package_dir = os.path.join(self.parent_component.package_dir, self.versioned_name)
        self.cache_dir = os.path.join(self.parent_component.cache_dir, self.versioned_name)
        self.work_dir = os.path.join(self.parent_component.work_dir, self.versioned_name)
    
    @property
    def extracted_dir(self) -> str:
        """Directory for extracted source code."""
        return self.src_dir
    
    @property
    def src_dir(self) -> str:
        """Version-specific source directory."""
        return os.path.join(self.work_dir, "src")
    
    @property
    def build_dir(self) -> str:
        """Out-of-tree build directory."""
        return os.path.join(self.src_dir, "build")

# ========================================================================
# 🔧 Concrete Component Classes
# ========================================================================

class APTComponent(Component):
    """APT packages management component (versionless)."""
    
    # APT-specific prerequisites with specialization
    download_prerequisites: List[str] = Component.download_prerequisites + [
        "apt-rdepends",                # Advanced dependency resolution and analysis tool
    ]
    
    def __init__(self):
        super().__init__("apt")
    
    @property
    def basic_tools(self) -> List[str]:
        """Basic tools from Dockerfile basic-tools stage."""
        return [
            "git",                         # For xmake initialize
        ]
    
    @property
    def all_packages(self) -> List[str]:
        """Complete package list for development container."""
        return self.basic_tools


class UVComponent(Component):
    """
    UV Python package manager component.
    
    Manages:
    • UV standalone binary distribution
    • Python interpreter installation (via UV's python management)
    • Python packages from pyproject.toml (wheels, dependencies)
    
    Cache Strategy:
    • cache_dir/ - UV tarball cache (Docker layer, for build efficiency)
      - Only caches UV binary tarball downloads
      - As uv do not provide download python packages separately, we have to give up caching python binary
      - Not mounted as volume, ensures Docker layer caching
    
    • package_dir/ - Multi-purpose directory for cross-stage transfer
      - UV binary for installation
      - Python installation cache (UV_CACHE_DIR during python install, cached the same directory with python packages)
      - Python packages cache (pip wheels, venv)
      - Transferred to expand-stage for installation
    
    Why this design:
    • UV tarball cached separately for Docker layer reuse
    • Python install uses package_dir as UV_CACHE_DIR (no separate download cache)
    • Python packages cache goes to standard location for later stages
    • All needed files in package_dir for expand-stage
    """
    
    base_url = "https://github.com/astral-sh/uv/releases/download/{version}"
    tarball_name_pattern = "uv-{machine}-unknown-linux-gnu.tar.gz"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["uv"]
        super().__init__("uv", version)
        # Python version managed by this UV instance
        self.python_version = TOOLCHAIN_VERSIONS["python"]
    
    @property
    def tarball_cache_dir(self) -> str:
        """Directory where UV binary tarball is cached (Docker layer)."""
        return os.path.join(self.cache_dir, "tarball")

    @property
    def tarball_package_dir(self) -> str:
        """Directory where UV binary tarball is cached (Docker layer)."""
        return os.path.join(self.package_dir, "tarball")
    
    @property
    def install_dir(self) -> str:
        """Directory where UV binary will be installed."""
        return "/root/.local/bin"
    
    @property
    def packages_package_dir(self) -> str:
        """
        UV_CACHE_DIR for Python installation phase.
        Points to package_dir to avoid separate cache layer.
        """
        return os.path.join(self.package_dir, "uv-packages")
    
class XMakeComponent(Component):
    """XMake build system component."""
    
    base_url = "https://github.com/xmake-io/xmake/releases/download/v{version}"
    tarball_name_pattern = "xmake-bundle-v{version}.{system}.{machine}"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["xmake"]
        super().__init__("xmake", version)
    
class CMakeComponent(Component):
    """CMake build system component."""
    
    base_url = "https://github.com/Kitware/CMake/releases/download/v{version}"
    tarball_name_pattern = "cmake-{version}-linux-x86_64.sh"
    verification_name_pattern = "cmake-{version}-SHA-256.txt"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["cmake"]
        super().__init__("cmake", version)

class ToolchainComponent(Component):
    """Main toolchain component containing glibc, gcc, llvm, linux sub-components."""
    
    def __init__(self):
        super().__init__("toolchain")
        
        # Create sub-components
        self.glibc: GlibcSubComponent = GlibcSubComponent(self)
        self.gcc: GccSubComponent = GccSubComponent(self)
        self.llvm: LlvmSubComponent = LlvmSubComponent(self)
        self.linux: LinuxSubComponent = LinuxSubComponent(self)

        # Sub-component registry
        self.sub_components: list[ToolchainSubComponent] = [
            self.glibc,
            self.gcc,
            self.llvm,
            self.linux,
        ]
        
    @property
    def sysroot_dir(self) -> str:
        """Sysroot directory with version-specific naming."""
        return f"{self.package_dir}/sysroot/{self.host_triplet}/{self.target_triplet}/glibc{self.glibc.version}-libstdc++{self.gcc.version}-linux{self.linux.version}"

# ========================================================================
# 🧩 Toolchain Sub-Component Classes
# ========================================================================

class GlibcSubComponent(ToolchainSubComponent):
    """GNU C Library sub-component."""
    
    base_url = "https://ftpmirror.gnu.org/gnu/glibc"
    tarball_name_pattern = "glibc-{version}.tar.xz"
    verification_name_pattern = "glibc-{version}.tar.xz.sig"
    build_prerequisites: List[str] = [
        "make",                        # GNU Make build automation
        "binutils",                    # Binary utilities (assembler, linker, etc.)
        "gawk",                        # Text processing (required by glibc build system)
        "bison",                       # Parser generator (required by glibc build system)
        "gcc-9",                       # GNU C compiler version 9 (for glibc < 2.36)

        *ToolchainSubComponent.build_prerequisites
    ]
    
    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("glibc", parent_component)


class GccSubComponent(ToolchainSubComponent):
    """GNU Compiler Collection sub-component."""
    
    base_url = "https://ftpmirror.gnu.org/gnu/gcc/gcc-{version}"
    tarball_name_pattern = "gcc-{version}.tar.xz"
    verification_name_pattern = "gcc-{version}.tar.xz.sig"
    build_prerequisites: List[str] = [
        "make",                        # GNU Make build automation
        "binutils",                    # Binary utilities (assembler, linker, etc.)
        "file",                        # File type identification (libcc1 requires this tool)
        
        "gcc-14",                      # Latest GNU C compiler (for bootstrapping)
        "g++-14",                      # Latest GNU C++ compiler (for bootstrapping)
        "libstdc++-14-dev",            # Multiple Precision Floating-Point Reliable Library

        *ToolchainSubComponent.build_prerequisites
    ]
    
    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("gcc", parent_component)
    
    @property
    def target_libs(self) -> List[str]:
        """Selective GCC library build targets."""
        return [
            "libgcc",         # Low-level runtime support library (exception handling, etc.)
            "libstdc++-v3",   # C++ standard library implementation
            "libsanitizer",   # Address/memory/thread sanitizer runtime libraries
            "libatomic",      # Atomic operations library for lock-free programming
            "libbacktrace",   # Stack backtrace support for debugging
            "libgomp",        # OpenMP parallel programming runtime
            "libquadmath"     # Quadruple precision math library
        ]


class LlvmSubComponent(ToolchainSubComponent):
    """LLVM Project sub-component."""
    
    base_url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}"
    tarball_name_pattern = "llvm-project-{version}.src.tar.xz"
    verification_name_pattern = "llvm-project-{version}.src.tar.xz.sig"
    
    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("llvm", parent_component)


class LinuxSubComponent(ToolchainSubComponent):
    """Linux Kernel Headers sub-component."""
    
    base_url = "https://github.com/torvalds/linux/archive/refs/tags"
    tarball_name_pattern = "v{version}.tar.gz"
    verification_name_pattern = ""  # Linux kernel releases don't include separate signature files
    build_prerequisites: List[str] = [
        "make",                        # GNU Make build automation
        "binutils",                    # Binary utilities (assembler, linker, etc.)
        "rsync",                       # File synchronization (Linux kernel headers)

        "gcc-9",                       # Even though we don't build the kernel, configure requires gcc

        *ToolchainSubComponent.build_prerequisites
    ]

    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("linux", parent_component)

class CliceSetupScriptsComponent(Component):
    """
    Clice setup scripts and configuration component.
    
    Contains Python scripts and configuration files needed for container setup,
    packaged as a complete directory structure:
    - config/build_config.py: Configuration definitions
    - config/default-toolchain-version.json: Version information
    - docker/linux/utility/local_setup.py: Final container setup script
    - docker/linux/utility/build_utils.py: Utility functions
    
    These files are packaged preserving their directory structure and will be
    executed in-place during container expansion (no extraction to CLICE_WORKDIR needed).
    """
    
    def __init__(self):
        super().__init__("clice-setup-scripts")
    
    @property
    def files_to_copy(self) -> list[str]:
        """List of files to copy with their relative paths (preserving directory structure)."""
        return [
            'config/build_config.py',
            'config/default-toolchain-version.json',
            'docker/linux/utility/local_setup.py',
            'docker/linux/utility/build_utils.py',
        ]

class BashrcComponent(Component):
    """
    Bash configuration component.
    
    Contains the .bashrc file with:
    - Environment variables (PATH, etc.)
    - Container entrypoint script (auto Python env setup)
    """
    
    def __init__(self):
        super().__init__("bashrc")
    
    @property
    def bashrc_path(self) -> str:
        """Path to .bashrc file in package."""
        return os.path.join(self.package_dir, ".bashrc")
    
    @property
    def entrypoint_script_source(self) -> str:
        """Path to container-entrypoint.sh source file."""
        return os.path.join(CLICE_WORKDIR, "docker/linux/container-entrypoint.sh") 

# ========================================================================
# 🏗️ Component Instances and Build Stage Organization
# ========================================================================

# Create component instances
APT = APTComponent()
UV = UVComponent()
XMAKE = XMakeComponent()
CMAKE = CMakeComponent()
TOOLCHAIN = ToolchainComponent()
CLICE_SETUP_SCRIPTS = CliceSetupScriptsComponent()
BASHRC = BashrcComponent()

# ========================================================================
# 📋 Build Stage Component Groups
# ========================================================================

# Dependencies downloader stage components
DEPENDENCIES_DOWNLOADER_STAGE: list[Component] =  [
    APT,
    UV,
    CMAKE,
    XMAKE,
]

# Image packer stage components (scripts and configs that go into package)
IMAGE_PACKER_STAGE: list[Component] = [
    CLICE_SETUP_SCRIPTS,
    BASHRC,
]

# Toolchain builder stage components
TOOLCHAIN_BUILDER_STAGE: list[Component] = [
    TOOLCHAIN,
]

# Master component registry
ALL_COMPONENTS = [
    *DEPENDENCIES_DOWNLOADER_STAGE,
    *IMAGE_PACKER_STAGE,
    *TOOLCHAIN_BUILDER_STAGE,
]


