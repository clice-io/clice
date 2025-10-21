# ========================================================================
# 🚀 Clice Local Setup
# ========================================================================
# File: docker/linux/utility/local_setup.py  
# Purpose: Final setup phase for the Clice development container
#
# This script handles the installation and configuration of all pre-downloaded
# packages and tools to complete the development environment setup.
# ========================================================================

"""
🚀 Clice Local Setup Script

Handles the final setup phase of the Clice development container by:
1. Installing all pre-downloaded APT packages
2. Extracting and installing the custom toolchain
3. Setting up CMake and XMake build systems
4. Installing Python packages via uv
5. Configuring environment variables and PATH

This script maximizes Docker build cache efficiency by separating the
installation phase from the download phase, allowing for independent
caching of each step.
"""

import os
import sys
import tarfile
import shutil

# Ensure utility directory is in Python path for imports
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

# Import all configuration from build_config using new component structure
from config.build_config import (
    RELEASE_PACKAGE_DIR,  
    CLICE_WORKDIR,
    # Import component instances for structured access
    APT, UV, CMAKE, XMAKE, TOOLCHAIN, BASHRC
)

# Import build_utils for run_command and other utilities
from build_utils import (
    Job,
    ParallelTaskScheduler,
    run_command
)

# ========================================================================
# 🌍 Environment Setup Functions
# ========================================================================

def deploy_bashrc():
    """Deploy .bashrc from package to /root/.bashrc."""
    print("🌍 Deploying .bashrc configuration...")
    
    source_bashrc = BASHRC.bashrc_path
    target_bashrc = "/root/.bashrc"
    
    if not os.path.exists(source_bashrc):
        print(f"⚠️  Warning: .bashrc not found at {source_bashrc}")
        return
    
    # Copy .bashrc to target location
    shutil.copy2(source_bashrc, target_bashrc)
    
    print(f"✅ .bashrc deployed to {target_bashrc}")

# ========================================================================
# 📦 Package Installation Functions
# ========================================================================

def install_apt_packages(apt_component):
    """Install APT development packages from downloaded .deb files."""
    print("📦 Installing APT development packages...")
    
    # Install all .deb files found in the package directory
    deb_files = [f for f in os.listdir(apt_component.package_dir) if f.endswith('.deb')]
    
    if deb_files:
        # Use dpkg to install all .deb files, with apt-get fallback for dependencies
        run_command(f"dpkg -i {apt_component.package_dir}/*.deb || apt-get install -f -y")
        print(f"✅ Installed {len(deb_files)} APT packages")
    else:
        print("⚠️ No .deb files found in APT package directory")

def install_toolchain(toolchain_component):
    """Install the custom toolchain."""
    print("🔧 Installing custom toolchain...")
    
    print(f"✅ Toolchain available at: {toolchain_component.package_dir}")

def install_cmake(cmake_component):
    """Install CMake from pre-downloaded installer."""
    print("🔧 Installing CMake...")
    
    cmake_installer_filename = cmake_component.tarball_name
    cmake_installer_path = os.path.join(cmake_component.package_dir, cmake_installer_filename)
    
    # Make installer executable and run it  
    run_command(f"chmod +x {cmake_installer_path}")
    
    # Use CMAKE component package_dir as install directory
    cmake_install_dir = cmake_component.package_dir
    os.makedirs(cmake_install_dir, exist_ok=True)
    
    # Install CMake to the component package directory
    run_command(f"{cmake_installer_path} --prefix={cmake_install_dir} --skip-license")
    
    # Create symlinks to system PATH
    cmake_bin_dir = f"{cmake_install_dir}/bin"
    if os.path.exists(cmake_bin_dir):
        for binary in os.listdir(cmake_bin_dir):
            src = os.path.join(cmake_bin_dir, binary)
            dst = f"/usr/local/bin/{binary}"
            if os.path.isfile(src) and not os.path.exists(dst):
                os.symlink(src, dst)
    
    print(f"✅ CMake installed to {cmake_install_dir}")

def install_xmake(xmake_component):
    """Install XMake from pre-downloaded package.""" 
    print("🔨 Installing XMake...")
    
    xmake_filename = xmake_component.tarball_name
    xmake_path = os.path.join(xmake_component.package_dir, xmake_filename)
    
    # Make XMake bundle executable
    run_command(f"chmod +x {xmake_path}")
    
    # Install XMake using update-alternatives
    run_command(f"update-alternatives --install /usr/bin/xmake xmake {xmake_path} 100")
    
    print("✅ XMake installed successfully")

def install_python_packages(uv_component):
    """Install Python packages from uv cache."""
    print("🐍 Installing Python packages...")
    
    # Install wheel files found in the UV package directory
    wheel_files = [f for f in os.listdir(uv_component.package_dir) if f.endswith('.whl')]
    
    if wheel_files:
        # Use uv to install from the cached packages
        wheel_paths = [os.path.join(uv_component.package_dir, f) for f in wheel_files]
        run_command(f"uv pip install --find-links {uv_component.package_dir} --no-index --force-reinstall --no-deps {' '.join(wheel_paths)}")
        print(f"✅ Installed {len(wheel_files)} Python packages")
    else:
        print("⚠️ No wheel files found in UV package directory")

# ========================================================================
# 📋 Setup Orchestration
# ========================================================================

def setup_git_safe_directory():
    """Configure git to treat the workspace as safe."""
    print("🔧 Configuring git safe directory...")
    
    run_command(f"git config --global --add safe.directory {CLICE_WORKDIR}")
    print("✅ Git safe directory configured")

def main():
    """Main setup orchestration function with parallel task scheduling."""
    print("🚀 Setting up Clice Dev Container...")
    
    # Define setup jobs with proper dependency management
    # Note: Release archive is already extracted by Dockerfile, so we start with installations
    jobs = {
        "setup_git_safe_directory": Job("setup_git_safe_directory", setup_git_safe_directory, ()),
        "install_apt_packages": Job("install_apt_packages", install_apt_packages, (APT,)),
        "install_toolchain": Job("install_toolchain", install_toolchain, (TOOLCHAIN,)),
        "install_cmake": Job("install_cmake", install_cmake, (CMAKE,)),
        "install_xmake": Job("install_xmake", install_xmake, (XMAKE,)),
        "install_python_packages": Job("install_python_packages", install_python_packages, (UV,)),
        "deploy_bashrc": Job("deploy_bashrc", deploy_bashrc, ()),
    }
    
    # Define dependencies - git setup depends on apt packages
    dependencies = {
        "install_apt_packages": set(),
        "setup_git_safe_directory": {"install_apt_packages"},
        "install_toolchain": set(),
        "install_cmake": set(),
        "install_xmake": set(),
        "install_python_packages": set(),
        "deploy_bashrc": set(),
    }
    
    # Execute setup tasks in parallel where possible
    scheduler = ParallelTaskScheduler(jobs, dependencies)
    scheduler.run()
    
    print("✅ Clice development environment setup completed successfully!")
    print(f"📦 Components installed from: {RELEASE_PACKAGE_DIR}")
    print(f"📝 Bashrc deployed to: /root/.bashrc")

if __name__ == "__main__":
    main()