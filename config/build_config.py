"""Centralized configuration constants for toolchain build system."""

import json
import os
from typing import Any, List, Dict

# ========================================================================
# 🌍 Environment Variables and Core Paths
# ========================================================================

# Global environment variables that will be used in dev-container shells
DEVELOPMENT_SHELL_VARS: Dict[str, str] = {
    "PATH": "/root/.local/bin:${PATH}",
    "XMAKE_ROOT": "y"
}

# Environment variables specifically for toolchain build processes
# NOT used in dev-container shells
TOOLCHAIN_BUILD_ENV_VARS: Dict[str, str] = {
    "ORIGIN": "$ORIGIN" # Enable relative rpath for portable binary distribution
}

# CLICE_WORKDIR is different from PROJECT_ROOT
# CLICE_WORKDIR means where the Clice project is mounted inside the container
CLICE_WORKDIR: str = os.getenv("CLICE_WORKDIR", "")
# PROJECT_ROOT is the root directory of the Clice project
# It could be different from CLICE_WORKDIR if the build_config.py is executed in expand-stage
PROJECT_ROOT: str = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
PYPROJECT_PATH: str = os.path.join(PROJECT_ROOT, "pyproject.toml")
TOOLCHAIN_CONFIG_PATH: str = os.path.join(PROJECT_ROOT, "config/default-toolchain-version.json")

# ========================================================================
# 📦 Release Package Configuration (Cross-Stage Variables)
# ========================================================================

# main package directory
RELEASE_PACKAGE_DIR: str = os.getenv("RELEASE_PACKAGE_DIR", "")
# compressed package archive
PACKED_RELEASE_PACKAGE_PATH: str = os.getenv("PACKED_RELEASE_PACKAGE_PATH", "")
# bashrc environment configuration file
ENVIRONMENT_CONFIG_FILE: str = os.getenv("ENVIRONMENT_CONFIG_FILE", "")
# File cache directory for toolchain build
# Only save what is always same, e.g. source code tarballs
BUILD_CACHE_DIR: str = os.getenv("BUILD_CACHE_DIR", "")
WORKDIR_ROOT: str = "/dev-container-build"  # Temporary work directory for builds (not persistent)

TOOLCHAIN_VERSIONS: Dict[str, Any] = {}
with open(TOOLCHAIN_CONFIG_PATH, "r") as f:
    TOOLCHAIN_VERSIONS = json.load(f)

# ========================================================================
# 🔐 GPG Verification Configuration
# ========================================================================

GPG_KEY_SERVER: List[str] = [
    "keys.openpgp.org",
    "keyserver.ubuntu.com"
]

# ========================================================================
# 🧩 Component Architecture Definitions
# ========================================================================

class Component:
    # Class-level URL patterns (overridden by subclasses)
    base_url: str = ""
    tarball_name_pattern: str = ""
    verification_name_pattern: str = ""
    
    # Class-level prerequisites configuration (overridden by subclasses)
    download_prerequisites: List[str] = [
        "aria2",                       # High-speed multi-connection downloader
        "gnupg",                       # For GPG signature verification, python-gnupg is just a wrapper
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
            
    @property
    def package_dir(self) -> str:
        return os.path.join(RELEASE_PACKAGE_DIR, self.versioned_name)

    @property
    def cache_dir(self) -> str:
        return os.path.join(BUILD_CACHE_DIR, self.versioned_name)

    @property
    def work_dir(self) -> str:
        return os.path.join(WORKDIR_ROOT, self.versioned_name)

    @property
    def versioned_name(self) -> str:
        return f"{self.name}-{self.version}"

    @property
    def tarball_name(self) -> str:
        if not self.tarball_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required tarball_name_pattern")

        return self.tarball_name_pattern.format(version=self.version, system=self.host_system, machine=self.host_machine)
        
    @property
    def tarball_url(self) -> str:
        if not self.base_url:
            raise ValueError(f"Component '{self.name}' missing required base_url")
        if not self.tarball_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required tarball_name_pattern")

        formatted_base_url = self.base_url.format(version=self.version, system=self.host_system, machine=self.host_machine)
        return f"{formatted_base_url}/{self.tarball_name}"

    @property
    def verification_name(self) -> str:
        if not self.verification_name_pattern:
            raise ValueError(f"Component '{self.name}' missing required verification_name_pattern")

        return self.verification_name_pattern.format(version=self.version, system=self.host_system, machine=self.host_machine)

    @property
    def verification_url(self) -> str:
        formatted_base_url = self.base_url.format(version=self.version, system=self.host_system, machine=self.host_machine)
        return f"{formatted_base_url}/{self.verification_name}"

    @property
    def host_triplet(self) -> str:
        return f"{self.host_machine}-{self.host_system}-gnu"
    
    @property
    def target_triplet(self) -> str:
        return f"{self.target_machine}-{self.target_system}-gnu"


class ToolchainSubComponent(Component):
    def __init__(self, name: str, parent_component: Component):
        version = TOOLCHAIN_VERSIONS[name]
        super().__init__(name, version)
        self.parent_component = parent_component
    
    @property
    def package_dir(self) -> str:
        return os.path.join(self.parent_component.package_dir, self.versioned_name)

    @property
    def cache_dir(self) -> str:
        return os.path.join(self.parent_component.cache_dir, self.versioned_name)

    @property
    def work_dir(self) -> str:
        return os.path.join(self.parent_component.work_dir, self.versioned_name)

    @property
    def extracted_dir(self) -> str:
        return self.src_dir
    
    @property
    def src_dir(self) -> str:
        return os.path.join(self.work_dir, "src")
    
    @property
    def build_dir(self) -> str:
        return os.path.join(self.src_dir, "build")

# ========================================================================
# 🔧 Concrete Component Classes
# ========================================================================

class APTComponent(Component):
    
    download_prerequisites: List[str] = Component.download_prerequisites + [
        "apt-rdepends",  # For resolving APT package dependencies
    ]
    
    def __init__(self):
        super().__init__("apt")

    @property
    def all_packages(self) -> List[str]:
        return [
            "git",                         # For XMake initialize
            "ca-certificates",             # For ssl verification, git needs it
            "ninja",                       # For CMake and XMake build system
            "curl",                        # For XMake initialize
            "make"                         # For XMake initialize
        ]


class UVComponent(Component):
    base_url = "https://github.com/astral-sh/uv/releases/download/{version}"
    tarball_name_pattern = "uv-{machine}-unknown-linux-gnu.tar.gz"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["uv"]
        super().__init__("uv", version)
        self.python_version = TOOLCHAIN_VERSIONS["python"]
    
    @property
    def tarball_cache_dir(self) -> str:
        return os.path.join(self.cache_dir, "tarball")

    @property
    def tarball_package_dir(self) -> str:
        return os.path.join(self.package_dir, "tarball")
    
    @property
    def install_dir(self) -> str:
        return "/root/.local/bin"
    
    @property
    def packages_package_dir(self) -> str:
        return os.path.join(self.package_dir, "uv-packages")
    
class XMakeComponent(Component):
    base_url = "https://github.com/xmake-io/xmake/releases/download/v{version}"
    tarball_name_pattern = "xmake-bundle-v{version}.{system}.{machine}"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["xmake"]
        super().__init__("xmake", version)
    
class CMakeComponent(Component):
    base_url = "https://github.com/Kitware/CMake/releases/download/v{version}"
    tarball_name_pattern = "cmake-{version}-linux-x86_64.sh"
    verification_name_pattern = "cmake-{version}-SHA-256.txt"
    
    def __init__(self):
        version = TOOLCHAIN_VERSIONS["cmake"]
        super().__init__("cmake", version)

class P7ZipComponent(Component):
    build_prerequisites: List[str] = [
        "p7zip-full",
    ]
    
    compression_level: str = "9"  # Maximum compression (0-9)
    
    def __init__(self):
        # p7zip doesn't have a version we track, using "system" as placeholder
        super().__init__("p7zip", "system")
    
    @property
    def compression_options(self) -> List[str]:
        return [
            f"-t7z",                         # Archive type
            f"-mx={self.compression_level}", # Compression level
            "-mmt=on",                       # Use all available CPU cores
            "-ms=on"                         # Better compression for similar files
        ]
    
    @property
    def sfx_option(self) -> str:
        return f"-sfx7zCon.sfx"

class ToolchainComponent(Component):
    
    def __init__(self):
        super().__init__("toolchain", "custom")
        
        self.glibc: GlibcSubComponent = GlibcSubComponent(self)
        self.gcc: GccSubComponent = GccSubComponent(self)
        self.llvm: LlvmSubComponent = LlvmSubComponent(self)
        self.linux: LinuxSubComponent = LinuxSubComponent(self)

        self.sub_components: list[ToolchainSubComponent] = [
            self.glibc,
            self.gcc,
            self.llvm,
            self.linux,
        ]
        
    @property
    def sysroot_dir(self) -> str:
        return f"{self.package_dir}/sysroot/{self.host_triplet}/{self.target_triplet}/glibc{self.glibc.version}-libstdc++{self.gcc.version}-linux{self.linux.version}"

# ========================================================================
# 🧩 Toolchain Sub-Component Classes
# ========================================================================

class GlibcSubComponent(ToolchainSubComponent):
    
    base_url = "https://ftpmirror.gnu.org/gnu/glibc"
    tarball_name_pattern = "glibc-{version}.tar.xz"
    verification_name_pattern = "glibc-{version}.tar.xz.sig"
    build_prerequisites: List[str] = [
        "make",
        "binutils",
        "gawk",                        # Text processing (required by glibc build system)
        "bison",                       # Parser generator (required by glibc build system)
        "gcc-9",                       # GNU C compiler version 9 (for glibc < 2.36)

        *ToolchainSubComponent.build_prerequisites
    ]
    
    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("glibc", parent_component)


class GccSubComponent(ToolchainSubComponent):
    
    base_url = "https://ftpmirror.gnu.org/gnu/gcc/gcc-{version}"
    tarball_name_pattern = "gcc-{version}.tar.xz"
    verification_name_pattern = "gcc-{version}.tar.xz.sig"
    build_prerequisites: List[str] = [
        "make",
        "binutils",
        "file",  # File type identification (libcc1 requires this tool)
        
        "gcc-14",
        "g++-14",
        "libstdc++-14-dev",

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
    base_url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}"
    tarball_name_pattern = "llvm-project-{version}.src.tar.xz"
    verification_name_pattern = "llvm-project-{version}.src.tar.xz.sig"
    
    download_prerequisites = ToolchainSubComponent.download_prerequisites + [
        "git",                         # Required for git clone llvm
    ]

    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("llvm", parent_component)


class LinuxSubComponent(ToolchainSubComponent):
    base_url = "https://github.com/torvalds/linux/archive/refs/tags"
    tarball_name_pattern = "v{version}.tar.gz"
    verification_name_pattern = ""
    build_prerequisites: List[str] = [
        "make",
        "binutils",
        "rsync",                       # File synchronization (Linux kernel headers)

        "gcc-9",                       # Even though we don't build the kernel, configure requires gcc

        *ToolchainSubComponent.build_prerequisites
    ]

    def __init__(self, parent_component: ToolchainComponent):
        super().__init__("linux", parent_component)

class CliceSetupScriptsComponent(Component):
    def __init__(self):
        super().__init__("clice-setup-scripts", "project")
    
    @property
    def files_to_copy(self) -> list[str]:
        return [
            'config/build_config.py',
            'config/default-toolchain-version.json',
            'docker/linux/utility/local_setup.py',
            'docker/linux/utility/build_utils.py',
        ]

class BashrcComponent(Component):
    """Bash configuration with environment variables and container entrypoint."""
    
    def __init__(self):
        super().__init__("bashrc", "project")
    
    @property
    def bashrc_path(self) -> str:
        return os.path.join(self.package_dir, ".bashrc")
    
    @property
    def entrypoint_script_source(self) -> str:
        return os.path.join(PROJECT_ROOT, "docker/linux/container-entrypoint.sh")

# ========================================================================
# 🏗️ Component Instances and Build Stage Organization
# ========================================================================

APT = APTComponent()
UV = UVComponent()
XMAKE = XMakeComponent()
CMAKE = CMakeComponent()
P7ZIP = P7ZipComponent()
TOOLCHAIN = ToolchainComponent()
BASHRC = BashrcComponent()

# ========================================================================
# 📋 Build Stage Component Groups
# ========================================================================

DEPENDENCIES_DOWNLOADER_STAGE: list[Component] =  [
    APT,
    UV,
    CMAKE,
    XMAKE,
]

IMAGE_PACKER_STAGE: list[Component] = [
    BASHRC,
]

TOOLCHAIN_BUILDER_STAGE: list[Component] = [
    TOOLCHAIN,
]

ALL_COMPONENTS = [
    *DEPENDENCIES_DOWNLOADER_STAGE,
    *IMAGE_PACKER_STAGE,
    *TOOLCHAIN_BUILDER_STAGE,
]
