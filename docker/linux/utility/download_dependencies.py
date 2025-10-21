# ========================================================================
# 📦 Clice Dependencies Downloader
# ========================================================================
# File: docker/linux/utility/download_dependencies.py
# Purpose: Download all dev-container dependencies without installing them
# 
# This module downloads all required packages, tools, and dependencies
# without installing them, maximizing Docker build cache efficiency.
# Downloaded packages are stored in cache directories for later installation.
# ========================================================================

"""
🚀 Clice Dependencies Downloader

Downloads all required development dependencies for the Clice dev container
without installing them. This approach maximizes Docker build cache efficiency
by separating the download phase from the installation phase.

Components Downloaded:
    • APT packages for development tools
    • CMake binary releases
    • XMake installation packages
    • Python packages and dependencies
    • LLVM prebuilt binaries (if available)

The downloaded packages are stored in structured cache directories that
can be efficiently copied and cached by Docker's build system.
"""

import os
import shutil
import sys
import subprocess
from typing import List

# Add project root to Python path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from build_utils import (
    Job,
    ParallelTaskScheduler,
    download_file,
    run_command,
    verify_sha256
)
from config.build_config import (
    RELEASE_PACKAGE_DIR,
    PYPROJECT_PATH,
    APT, UV, CMAKE, XMAKE
)

# ========================================================================
# 🛠️ Download Task Implementations  
# ========================================================================

def install_download_prerequisites() -> None:
    """Install prerequisites required for downloading dependencies."""
    print("📦 Installing dependencies download prerequisites...")
    
    # Update package lists first  
    run_command("apt update -o DPkg::Lock::Timeout=-1")
    
    # Install all download prerequisites (universal + APT-specific)
    download_prerequisites = APT.download_prerequisites
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {' '.join(download_prerequisites)}")
    
    print(f"✅ Installed {len(download_prerequisites)} download prerequisites")

def get_apt_package_list(base_packages: List[str]) -> List[str]:
    """
    Get all required APT packages using the exact StackOverflow command pattern.
    
    Uses: apt-cache depends --recurse ... | awk '$1 ~ /^Depends:/{print $2}'
    Returns: Deduplicated list of packages to download
    """
    print("🔍 Resolving recursive dependencies using StackOverflow command pattern...")
    
    all_packages = set()
    
    for package in base_packages:
        try:
            # Use the exact command from StackOverflow
            apt_cache_cmd = [
                "apt-cache", "depends", "--recurse", "--no-recommends", 
                "--no-suggests", "--no-conflicts", "--no-breaks", 
                "--no-replaces", "--no-enhances", package
            ]
            
            # Run apt-cache depends command
            result = subprocess.run(apt_cache_cmd, capture_output=True, text=True, check=True)
            
            # Use awk pattern to extract dependency packages: $1 ~ /^Depends:/{print $2}
            for line in result.stdout.split('\n'):
                line = line.strip()
                if line.startswith('Depends:'):
                    # Extract the package name after "Depends: "
                    parts = line.split()
                    if len(parts) >= 2:
                        pkg_name = parts[1]
                        # Remove architecture suffix and version constraints
                        pkg_name = pkg_name.split(':')[0].split('(')[0].split('[')[0].strip()
                        if pkg_name and not pkg_name.startswith('<') and pkg_name != '|':
                            all_packages.add(pkg_name)
                        
        except subprocess.CalledProcessError as e:
            print(f"⚠️ Warning: Could not resolve dependencies for {package}: {e}")
            # Add the original package as fallback
            all_packages.add(package)
    
    # Filter available packages (remove virtual/unavailable packages)
    print(f"🔍 Found {len(all_packages)} total dependency packages, filtering available ones...")
    available_packages = []
    
    for package in sorted(all_packages):
        try:
            # Quick availability check
            result = subprocess.run(
                ["apt-cache", "show", package],
                capture_output=True, text=True, check=True
            )
            if result.returncode == 0:
                available_packages.append(package)
        except subprocess.CalledProcessError:
            # Skip unavailable packages
            continue
    
    print(f"📋 Final package list: {len(available_packages)} available packages")
    return available_packages

def download_apt_packages() -> None:
    """
    Download all APT packages using the exact StackOverflow command pattern.
    
    Two-stage approach:
    1. Get package list using apt-cache depends + awk pattern
    2. Download packages using apt-get download
    """
    print("📦 Downloading APT packages with StackOverflow command pattern...")
    
    # Create both download cache and package directories using component structure
    os.makedirs(APT.cache_dir, exist_ok=True)
    os.makedirs(APT.package_dir, exist_ok=True)
    
    # Stage 1: Get package list
    base_packages = list(set(APT.all_packages))
    print(f"📋 Base packages from config: {len(base_packages)} packages")
    
    available_packages = get_apt_package_list(base_packages)
    
    # Stage 2: Download packages using apt-get download
    print(f"📥 Downloading {len(available_packages)} packages to cache: {APT.cache_dir}")
    

    # Use the exact pattern: apt-get download $(package_list)
    # Split into batches to avoid command line length limits
    batch_size = 50
    downloaded_count = 0
    
    for i in range(0, len(available_packages), batch_size):
        batch = available_packages[i:i + batch_size]
        
        print(f"📦 Downloading batch {i//batch_size + 1}/{(len(available_packages) + batch_size - 1)//batch_size} ({len(batch)} packages)...")
        
        try:
            # Run apt-get download for this batch
            result = subprocess.run(
                ["apt-get", "download"] + batch,
                cwd=APT.cache_dir, 
                capture_output=True, 
                text=True, 
                check=True
            )
            downloaded_count += len(batch)
            
        except subprocess.CalledProcessError as e:
            print(f"⚠️ Batch download failed, trying individual packages...")
            # Fallback: download packages individually
            for package in batch:
                try:
                    subprocess.run(
                        ["apt-get", "download", package],
                        cwd=APT.cache_dir,
                        capture_output=True,
                        text=True,
                        check=True
                    )
                    downloaded_count += 1
                except subprocess.CalledProcessError:
                    print(f"⚠️ Failed to download {package}")
    
    print(f"✅ Downloaded {downloaded_count} packages to cache")
    
    # Copy packages from cache to package directory (only available_packages)
    print("📦 Copying packages from cache to package directory...")
    copied_count = 0
    
    # Create a set of expected package prefixes for efficient lookup
    package_prefixes = set()
    for pkg in available_packages:
        package_prefixes.add(pkg + "_")
    
    for file in os.listdir(APT.cache_dir):
        if file.endswith('.deb'):
            # Check if this .deb file corresponds to one of our available packages
            file_matches = any(file.startswith(prefix) for prefix in package_prefixes)
            if file_matches:
                src = os.path.join(APT.cache_dir, file)
                dst = os.path.join(APT.package_dir, file)
                shutil.copy2(src, dst)
                copied_count += 1
    
    print(f"✅ APT packages ready in {APT.package_dir}")
    print(f"📊 Copied {copied_count} packages from cache")
    print(f"📁 Cache directory: {APT.cache_dir} (preserved for future builds)")

def download_cmake() -> None:
    """Download CMake binary release with verification files and verify integrity."""
    print("🔧 Downloading CMake with verification...")
    
    # Create both cache and package directories using component structure
    os.makedirs(CMAKE.cache_dir, exist_ok=True)
    os.makedirs(CMAKE.package_dir, exist_ok=True)
    
    # Use CMake component configuration
    cmake_filename = CMAKE.tarball_name
    cmake_url = CMAKE.tarball_url
    
    # Download to cache directory first
    cmake_cache_file = f"{CMAKE.cache_dir}/{cmake_filename}"
    cmake_package_file = f"{CMAKE.package_dir}/{cmake_filename}"
    
    # Download CMake installer (.sh script) to cache
    download_file(cmake_url, cmake_cache_file)
    
    # Download verification files to cache using component structure
    sha_url = CMAKE.verification_url
    sha_filename = CMAKE.verification_name
    sha_cache_file = f"{CMAKE.cache_dir}/{sha_filename}"
    
    # Download SHA file for integrity verification
    download_file(sha_url, sha_cache_file)
    
    # Verify CMake file integrity using build_utils
    with open(sha_cache_file, 'r') as f:
        sha_content = f.read().strip()
        # Parse SHA file format: "hash  filename"
        for line in sha_content.split('\n'):
            if cmake_filename in line:
                expected_hash = line.split()[0]
                if verify_sha256(cmake_cache_file, expected_hash):
                    print("✅ CMake file integrity verification successful")
                else:
                    print("❌ CMake file integrity verification failed")
                    # Delete all files in cache directory on verification failure
                    shutil.rmtree(CMAKE.cache_dir, ignore_errors=True)
                    raise RuntimeError("CMake file integrity verification failed")
                break
        else:
            print("⚠️ CMake file not found in SHA file, skipping verification")
    
    # Copy verified file from cache to package directory
    shutil.copy2(cmake_cache_file, cmake_package_file)
    
    print(f"✅ CMake downloaded to cache: {cmake_cache_file}")
    print(f"📦 CMake copied to package: {cmake_package_file}")

def download_xmake() -> None:
    """Download XMake bundle for direct installation."""
    print("🔨 Downloading XMake bundle...")
    
    # Create both cache and package directories using component structure
    os.makedirs(XMAKE.cache_dir, exist_ok=True)
    os.makedirs(XMAKE.package_dir, exist_ok=True)
    
    # Use XMake component configuration
    xmake_filename = XMAKE.tarball_name
    xmake_url = XMAKE.tarball_url
    
    # Download to cache directory first
    xmake_cache_file = f"{XMAKE.cache_dir}/{xmake_filename}"
    xmake_package_file = f"{XMAKE.package_dir}/{xmake_filename}"
    
    # Download XMake bundle to cache
    download_file(xmake_url, xmake_cache_file)
    
    # Make it executable in cache
    os.chmod(xmake_cache_file, 0o755)
    
    # Copy from cache to package directory
    shutil.copy2(xmake_cache_file, xmake_package_file)
    
    print(f"✅ XMake downloaded to cache: {xmake_cache_file}")
    print(f"📦 XMake copied to package: {xmake_package_file}")

def download_python_packages() -> None:
    """
    Download Python packages from pyproject.toml using uv sync.
    
    Uses uv sync to download all dependencies to UV's packages cache directory.
    """
    print("🐍 Downloading Python packages from pyproject.toml...")
    
    # Create cache directory for packages
    os.makedirs(UV.packages_package_dir, exist_ok=True)
    
    # Set UV_CACHE_DIR to packages cache directory
    print(f"📥 Downloading package wheels to UV packages package dir: {UV.packages_package_dir}")
    print(f"📋 Using pyproject.toml from: {PYPROJECT_PATH}")
    
    # Run uv sync with project root as working directory
    # UV will automatically find pyproject.toml in the project root
    project_root = os.path.dirname(PYPROJECT_PATH)
    
    run_command(
        f"UV_CACHE_DIR={UV.packages_package_dir} uv sync --no-install-project --no-editable",
        cwd=project_root
    )

    print(f"✅ Package wheels cached to: {UV.packages_package_dir}")
    print(f"📁 Packages cache will be available to later stages via cache mount")

# LLVM downloading removed as per requirements

# ========================================================================
# 🚀 Main Execution
# ========================================================================

def main():
    """Main execution function with parallel task scheduling."""
    print("🚀 Starting Clice Dependencies Download Process...")
    
    # Create main cache directory
    os.makedirs(RELEASE_PACKAGE_DIR, exist_ok=True)
    
    # Define download jobs with proper dependency management
    # Note: Python installation is now done in Dockerfile, not here
    jobs = {
        "install_download_prerequisites": Job("install_download_prerequisites", install_download_prerequisites, ()),
        "download_apt_packages": Job("download_apt_packages", download_apt_packages, ()),
        "download_python_packages": Job("download_python_packages", download_python_packages, ()),
        "download_cmake": Job("download_cmake", download_cmake, ()),
        "download_xmake": Job("download_xmake", download_xmake, ()),
    }
    
    # Define dependencies
    # UV and packages downloads need install_download_prerequisites
    # Python installation is handled in Dockerfile base-stage
    dependencies = {
        "install_download_prerequisites": set(),
        "download_apt_packages": {"install_download_prerequisites"},
        "download_python_packages": {"install_download_prerequisites"},
        "download_cmake": {"install_download_prerequisites"},
        "download_xmake": {"install_download_prerequisites"},
    }
    
    # Execute downloads in parallel where possible
    scheduler = ParallelTaskScheduler(jobs, dependencies)
    scheduler.run()
    
    print("✅ All dependencies downloaded successfully!")
    print(f"📁 Cache directory: {RELEASE_PACKAGE_DIR}")

if __name__ == "__main__":
    main()
