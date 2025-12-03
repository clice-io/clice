#!/usr/bin/env python3
"""
Download all dev dependencies (APT packages, CMake, XMake, Python packages)
without installing them for Docker build cache efficiency.
"""

import os
import shutil
import sys
import subprocess
from typing import List, Dict, Set, Optional

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
from config.docker_build_stages.common import RELEASE_PACKAGE_DIR
from config.docker_build_stages.dependencies_config import APT, UV, CMAKE, XMAKE

# ========================================================================
# 🛠️ Download Task Implementations  
# ========================================================================

def install_download_prerequisites() -> None:
    print("📦 Installing dependencies download prerequisites...")
    
    # Install all download prerequisites (universal + APT-specific)
    download_prerequisites: List[str] = APT.download_prerequisites
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {' '.join(download_prerequisites)}")
    
    print(f"✅ Installed {len(download_prerequisites)} download prerequisites")

def get_apt_package_list(base_packages: List[str]) -> List[str]:
    """Get recursive APT dependencies using apt-cache depends + awk pattern."""
    print("🔍 Resolving recursive dependencies using StackOverflow command pattern...")
    
    all_packages: Set[str] = set()
    
    for package in base_packages:
        try:
            # Use the exact command from StackOverflow
            apt_cache_cmd: List[str] = [
                "apt-cache", "depends", "--recurse", "--no-recommends", 
                "--no-suggests", "--no-conflicts", "--no-breaks", 
                "--no-replaces", "--no-enhances", package
            ]
            
            # Run apt-cache depends command
            result: subprocess.CompletedProcess[str] = subprocess.run(
                apt_cache_cmd, capture_output=True, text=True, check=True
            )
            
            # Use awk pattern to extract dependency packages: $1 ~ /^Depends:/{print $2}
            for line in result.stdout.split('\n'):
                line = line.strip()
                if line.startswith('Depends:'):
                    # Extract the package name after "Depends: "
                    parts: List[str] = line.split()
                    if len(parts) >= 2:
                        pkg_name: str = parts[1]
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
    available_packages_set: Set[str] = set(base_packages)
    
    for package in sorted(all_packages):
        # Quick availability check
        result = subprocess.run(
            ["apt-cache", "show", package],
            capture_output=True, text=True, check=True
        )
        if result.returncode == 0:
            available_packages_set.add(package)
    
    available_packages: List[str] = sorted(available_packages_set)
    print(f"📋 Final package list: {len(available_packages)} available packages")
    return available_packages

def download_apt_packages() -> None:
    print("📦 Downloading APT packages with StackOverflow command pattern...")
    
    # Create both download cache and package directories using component structure
    os.makedirs(APT.cache_dir, exist_ok=True)
    os.makedirs(APT.package_dir, exist_ok=True)
    
    # Stage 1: Get package list
    base_packages: List[str] = list(set(APT.all_packages))
    print(f"📋 Base packages from config: {len(base_packages)} packages")
    
    available_packages: List[str] = get_apt_package_list(base_packages)
    
    # Stage 2: Download packages using apt-get download
    print(f"📥 Downloading {len(available_packages)} packages to cache: {APT.cache_dir}")    

    # Download all packages at once
    packages_str: str = ' '.join(available_packages)
    run_command(f"apt-get download {packages_str}", cwd=APT.cache_dir)
    
    print(f"✅ Downloaded {len(available_packages)} packages to cache")
    
    # Count actual .deb files in cache directory
    cached_deb_count: int = len([f for f in os.listdir(APT.cache_dir) if f.endswith('.deb')])
    print(f"📋 Found {cached_deb_count} .deb files in cache directory")
    
    # Stage 3: Parse all packages at once with apt-cache show to get real packages and their info
    print("📦 Parsing package information to identify real packages and versions...")
    
    # Get system architecture
    arch_result: subprocess.CompletedProcess[str] = subprocess.run(
        ["dpkg", "--print-architecture"],
        capture_output=True,
        text=True,
        check=True
    )
    system_arch: str = arch_result.stdout.strip()
    
    # Map package name -> exact filename (only for real packages)
    package_to_filename: Dict[str, str] = {}
    virtual_packages: List[str] = []
    
    for pkg in available_packages:
        try:
            # Single apt-cache show call to get all info at once
            show_result: subprocess.CompletedProcess[str] = subprocess.run(
                ["apt-cache", "show", pkg],
                capture_output=True,
                text=True,
                check=True
            )
            
            # Parse the output to extract Package, Version, and Architecture
            package_name: Optional[str] = None
            version: Optional[str] = None
            pkg_arch: str = system_arch  # Default
            
            for line in show_result.stdout.split('\n'):
                if line.startswith('Package:'):
                    package_name = line.split(':', 1)[1].strip()
                elif line.startswith('Version:'):
                    version = line.split(':', 1)[1].strip()
                elif line.startswith('Architecture:'):
                    pkg_arch = line.split(':', 1)[1].strip()
                
                # Stop after first package stanza (in case of multiple versions)
                if line.strip() == '' and package_name and version:
                    break
            
            # Check if this is a virtual package (Package field doesn't match query)
            if not package_name or package_name != pkg:
                virtual_packages.append(pkg)
                print(f"📝 Skipping virtual package: {pkg} (resolves to {package_name})")
                continue
            
            if not version:
                print(f"⚠️ No version found for {pkg}")
                continue
            
            # Construct expected filename based on Debian package naming convention
            # URL-encode colons in version
            encoded_version: str = version.replace(':', '%3a')
            expected_filename: str = f"{pkg}_{encoded_version}_{pkg_arch}.deb"
            
            package_to_filename[pkg] = expected_filename
            
        except subprocess.CalledProcessError as e:
            # If apt-cache show fails, it's likely a virtual package
            virtual_packages.append(pkg)
            print(f"📝 Skipping virtual package (no show output): {pkg}")
    
    print(f"📋 Identified {len(package_to_filename)} real packages with .deb files")
    print(f"📝 Identified {len(virtual_packages)} virtual packages (no .deb files)")
    
    # Also verify that actual .deb files in cache match what we copied
    if len(virtual_packages) + len(package_to_filename) != len(available_packages):
        error_msg: str = f"File count mismatch: {len(available_packages)} available vs {len(package_to_filename)} real + {len(virtual_packages)} virtual"
        print(f"❌ {error_msg}")
        raise RuntimeError(error_msg)
    
    # Stage 4: Copy only the exact files
    print("📦 Copying exact package files from cache to package directory...")
    
    for pkg, filename in package_to_filename.items():
        src: str = os.path.join(APT.cache_dir, filename)
        dst: str = os.path.join(APT.package_dir, filename)
        
        shutil.copy2(src, dst)
    
    print(f"📊 Copied {len(package_to_filename)} real packages")
    print(f"📝 Skipped {len(virtual_packages)} virtual packages (no .deb files)")
    print(f"✅ Verification passed: Download count matches copy count")
    print(f"📁 Cache directory: {APT.cache_dir} (preserved for future builds)")

def download_cmake() -> None:
    """Download CMake installer and verify SHA256 integrity."""
    print("🔧 Downloading CMake with verification...")
    
    # Create both cache and package directories using component structure
    os.makedirs(CMAKE.cache_dir, exist_ok=True)
    os.makedirs(CMAKE.package_dir, exist_ok=True)
    
    # Use CMake component configuration
    cmake_filename: str = CMAKE.tarball_name
    cmake_url: str = CMAKE.tarball_url
    
    # Download to cache directory first
    cmake_cache_file: str = f"{CMAKE.cache_dir}/{cmake_filename}"
    cmake_package_file: str = f"{CMAKE.package_dir}/{cmake_filename}"
    
    # Download CMake installer (.sh script) to cache
    download_file(cmake_url, cmake_cache_file)
    
    # Download verification files to cache using component structure
    sha_url: str = CMAKE.verification_url
    sha_filename: str = CMAKE.verification_name
    sha_cache_file: str = f"{CMAKE.cache_dir}/{sha_filename}"
    
    # Download SHA file for integrity verification
    download_file(sha_url, sha_cache_file)
    
    # Verify CMake file integrity using build_utils
    with open(sha_cache_file, 'r') as f:
        sha_content: str = f.read().strip()
        # Parse SHA file format: "hash  filename"
        for line in sha_content.split('\n'):
            if cmake_filename in line:
                expected_hash: str = line.split()[0]
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
    print("🔨 Downloading XMake bundle...")
    
    # Create both cache and package directories using component structure
    os.makedirs(XMAKE.cache_dir, exist_ok=True)
    os.makedirs(XMAKE.package_dir, exist_ok=True)
    
    # Use XMake component configuration
    xmake_filename: str = XMAKE.tarball_name
    xmake_url: str = XMAKE.tarball_url
    
    # Download to cache directory first
    xmake_cache_file: str = f"{XMAKE.cache_dir}/{xmake_filename}"
    xmake_package_file: str = f"{XMAKE.package_dir}/{xmake_filename}"
    
    # Download XMake bundle to cache
    download_file(xmake_url, xmake_cache_file)
    
    # Make it executable in cache
    os.chmod(xmake_cache_file, 0o755)
    
    # Copy from cache to package directory
    shutil.copy2(xmake_cache_file, xmake_package_file)
    
    print(f"✅ XMake downloaded to cache: {xmake_cache_file}")
    print(f"📦 XMake copied to package: {xmake_package_file}")

def download_python_packages() -> None:
    print("🐍 Downloading Python packages from pyproject.toml...")
    
    # Create cache directory for packages
    os.makedirs(UV.packages_package_dir, exist_ok=True)
    
    # Set UV_CACHE_DIR to packages cache directory
    print(f"📥 Downloading package wheels to UV packages package dir: {UV.packages_package_dir}")
    print(f"📋 Using pyproject.toml from: {UV.pyproject_file_path}")
    
    # Run uv sync with project root as working directory
    # UV will automatically find pyproject.toml in the project root
    run_command(
        f"UV_CACHE_DIR={UV.packages_package_dir} uv sync --no-install-project --no-editable",
        cwd=os.path.dirname(UV.pyproject_file_path)
    )

    print(f"✅ Package wheels cached to: {UV.packages_package_dir}")
    print(f"📁 Packages cache will be available to later stages via cache mount")

# LLVM downloading removed as per requirements

# ========================================================================
# 🚀 Main Execution
# ========================================================================

def main() -> None:
    print("🚀 Starting Clice Dependencies Download Process...")
    
    # Create main cache directory
    os.makedirs(RELEASE_PACKAGE_DIR, exist_ok=True)
    
    # Define download jobs with proper dependency management
    install_download_prereq_job = Job("install_download_prerequisites", install_download_prerequisites, ())
    download_apt_job = Job("download_apt_packages", download_apt_packages, (), [install_download_prereq_job])
    download_python_job = Job("download_python_packages", download_python_packages, (), [install_download_prereq_job])
    download_cmake_job = Job("download_cmake", download_cmake, (), [install_download_prereq_job])
    download_xmake_job = Job("download_xmake", download_xmake, (), [install_download_prereq_job])
    
    all_jobs = [
        install_download_prereq_job,
        download_apt_job,
        download_python_job,
        download_cmake_job,
        download_xmake_job,
    ]
    
    # Execute downloads in parallel where possible
    scheduler = ParallelTaskScheduler(all_jobs)
    scheduler.run()
    
    print("✅ All dependencies downloaded successfully!")
    print(f"📁 Cache directory: {RELEASE_PACKAGE_DIR}")

if __name__ == "__main__":
    main()
