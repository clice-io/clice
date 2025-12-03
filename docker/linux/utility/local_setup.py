#!/usr/bin/env python3
"""
Final setup: Install pre-downloaded packages (APT, toolchain, CMake, XMake, Python)
and deploy .bashrc configuration.
"""

from typing import List, Optional
import os
import sys
import shutil

# Ensure utility directory is in Python path for imports
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from config.docker_build_stages.common import (
    COMPILER,
    RELEASE_PACKAGE_DIR,  
    CLICE_WORKDIR,
    TOOLCHAIN_VERSIONS,
)
from config.docker_build_stages.dependencies_config import APT, UV, CMAKE, XMAKE, APTComponent, CMakeComponent, UVComponent, XMakeComponent
from config.docker_build_stages.toolchain_config import TOOLCHAIN, ToolchainComponent
from config.docker_build_stages.package_config import BASHRC

from build_utils import (
    Job,
    ParallelTaskScheduler,
    run_command
)

# ========================================================================
# 🌍 Environment Setup Functions
# ========================================================================

def deploy_bashrc() -> None:
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

def install_apt_packages(apt_component: APTComponent) -> None:
    print("📦 Installing APT development packages...")
    
    # Install all .deb files found in the package directory
    deb_files = [f for f in os.listdir(apt_component.package_dir) if f.endswith('.deb')]
    
    if deb_files:
        # Use dpkg to install all .deb files, with apt-get fallback for dependencies
        run_command(f"dpkg -i {apt_component.package_dir}/*.deb || apt-get install -f -y")
        print(f"✅ Installed {len(deb_files)} APT packages")
    else:
        print("⚠️ No .deb files found in APT package directory")

def install_toolchain(toolchain_component: ToolchainComponent) -> None:
    print("🔧 Installing custom toolchain...")
    
    match COMPILER:
        case "gcc":
            run_command(f'update-alternatives --install /usr/bin/cc cc "/usr/bin/gcc-{TOOLCHAIN_VERSIONS["gcc"]}" {TOOLCHAIN_VERSIONS["gcc"]}')
            run_command(f'update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-{TOOLCHAIN_VERSIONS["gcc"]}" {TOOLCHAIN_VERSIONS["gcc"]}')
            run_command(f'update-alternatives --install /usr/bin/c++ c++ "/usr/bin/g++-{TOOLCHAIN_VERSIONS["gcc"]}" {TOOLCHAIN_VERSIONS["gcc"]}')
            run_command(f'update-alternatives --install /usr/bin/g++ g++ "/usr/bin/g++-{TOOLCHAIN_VERSIONS["gcc"]}" {TOOLCHAIN_VERSIONS["gcc"]}')
        case "clang":
            run_command(f'update-alternatives --install /usr/bin/cc cc "/usr/bin/clang-{TOOLCHAIN_VERSIONS["clang"]}" {TOOLCHAIN_VERSIONS["clang"]}')
            run_command(f'update-alternatives --install /usr/bin/clang clang "/usr/bin/clang-{TOOLCHAIN_VERSIONS["clang"]}" {TOOLCHAIN_VERSIONS["clang"]}')
            run_command(f'update-alternatives --install /usr/bin/c++ c++ "/usr/bin/clang++-{TOOLCHAIN_VERSIONS["clang"]}" {TOOLCHAIN_VERSIONS["clang"]}')
            run_command(f'update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-{TOOLCHAIN_VERSIONS["clang"]}" {TOOLCHAIN_VERSIONS["clang"]}')
        case "zig":
            # Zig binary is directly in package_dir after stripping top-level directory
            zig_bin = os.path.join(toolchain_component.zig.package_dir, 'zig')
            
            # Setup alternatives for zig, cc, and c++
            run_command(f'update-alternatives --install /usr/bin/zig zig "{zig_bin}" 100')
            # run_command(f'update-alternatives --install /usr/bin/cc cc "{zig_bin} cc" 100')
            # run_command(f'update-alternatives --install /usr/bin/c++ c++ "{zig_bin} c++" 100')
            print(f"✅ Zig compiler configured: {zig_bin}")
        case _:
            raise ValueError(f"Unsupported compiler specified: {COMPILER}")

    # clice requires to link with lld
    run_command(f'update-alternatives --install /usr/bin/ld ld "/usr/bin/lld-{TOOLCHAIN_VERSIONS["clang"]}" {TOOLCHAIN_VERSIONS["clang"]}')

    print(f"✅ Toolchain available at: {toolchain_component.package_dir}")

def install_cmake(cmake_component: CMakeComponent) -> None:
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

def install_xmake(xmake_component: XMakeComponent) -> None:
    """Install XMake from pre-downloaded package.""" 
    print("🔨 Installing XMake...")
    
    xmake_filename = xmake_component.tarball_name
    xmake_path = os.path.join(xmake_component.package_dir, xmake_filename)
    
    # Make XMake bundle executable
    run_command(f"chmod +x {xmake_path}")
    
    # Install XMake using update-alternatives
    run_command(f"update-alternatives --install /usr/bin/xmake xmake {xmake_path} 100")

    # Run XMake
    # First time we execute the bundle, it sets up its internal environment
    # Environment variable is not setup yet, so we need --root option to bypass xmake root account check
    run_command("xmake --root --version")
    
    print("✅ XMake installed successfully")

def install_python_packages(uv_component: UVComponent) -> None:
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

def setup_git_safe_directory() -> None:
    """Configure git to treat the workspace as safe."""
    print("🔧 Configuring git safe directory...")
    
    run_command(f"git config --global --add safe.directory {CLICE_WORKDIR}")
    print("✅ Git safe directory configured")

def main() -> None:
    print("🚀 Setting up Clice Dev Container...")
    
    # Define setup jobs with proper dependency management
    install_apt_job = Job("install_apt_packages", install_apt_packages, (APT,))
    setup_git_job = Job("setup_git_safe_directory", setup_git_safe_directory, (), [install_apt_job])
    install_toolchain_job = Job("install_toolchain", install_toolchain, (TOOLCHAIN,), [install_apt_job])
    install_cmake_job = Job("install_cmake", install_cmake, (CMAKE,))
    install_xmake_job = Job("install_xmake", install_xmake, (XMAKE,))
    install_python_job = Job("install_python_packages", install_python_packages, (UV,))
    deploy_bashrc_job = Job("deploy_bashrc", deploy_bashrc, ())
    
    all_jobs = [
        install_apt_job,
        setup_git_job,
        install_toolchain_job,
        install_cmake_job,
        install_xmake_job,
        install_python_job,
        deploy_bashrc_job,
    ]
    
    # Execute setup tasks in parallel where possible
    scheduler = ParallelTaskScheduler(all_jobs)
    scheduler.run()
    
    print("✅ Clice development environment setup completed successfully!")
    print(f"📦 Components installed from: {RELEASE_PACKAGE_DIR}")
    print(f"📝 Bashrc deployed to: /root/.bashrc")

if __name__ == "__main__":
    main()