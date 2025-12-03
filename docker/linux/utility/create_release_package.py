#!/usr/bin/env python3
"""
Stage 3: Create final release package by merging toolchain and dependencies,
generating manifest, and packaging into 7z SFX archive.
"""

import os
import sys
import json

# Add project root to Python path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from config.docker_build_stages.common import (
    COMPILER,
    RELEASE_PACKAGE_DIR,
    PACKED_RELEASE_PACKAGE_PATH,
    CLICE_WORKDIR,
    DEVELOPMENT_SHELL_VARS,
    TOOLCHAIN_VERSIONS,
)
from config.docker_build_stages.toolchain_config import TOOLCHAIN
from config.docker_build_stages.dependencies_config import UV
from config.docker_build_stages.package_config import (
    ALL_COMPONENTS,
    BASHRC,
    P7ZIP,
)

from build_utils import (
    Job,
    ParallelTaskScheduler,
    run_command
)

# ========================================================================
# 🌍 Environment Setup Functions
# ========================================================================

def setup_environment_variables_and_entrypoint() -> None:
    """Create .bashrc with environment variables and container entrypoint script."""
    print("🌍 Setting up .bashrc with environment variables and entrypoint script...")
    
    # Read container entrypoint script from BashrcComponent
    entrypoint_script_path = BASHRC.entrypoint_script_source
    
    with open(entrypoint_script_path, 'r') as f:
        entrypoint_content = f.read()
    
    # Create .bashrc in BASHRC component package directory
    bashrc_path = BASHRC.bashrc_path
    os.makedirs(os.path.dirname(bashrc_path), exist_ok=True)
    
    # Write complete .bashrc
    with open(bashrc_path, 'w') as f:
        f.write("# ========================================================================\n")
        f.write("# 🚀 Clice Dev Container - Bash Configuration\n")
        f.write("# ========================================================================\n")
        f.write("# This file is auto-generated during image packaging.\n")
        f.write("# It contains:\n")
        f.write("#   1. Exported environment variables from DEVELOPMENT_SHELL_VARS\n")
        f.write("#   2. Internal variables for container entrypoint (not exported)\n")
        f.write("#   3. Container entrypoint script (auto Python environment setup)\n")
        f.write("# ========================================================================\n\n")
        
        # Export environment variables from DEVELOPMENT_SHELL_VARS
        f.write("# Exported environment variables (from DEVELOPMENT_SHELL_VARS)\n")
        for key, value in DEVELOPMENT_SHELL_VARS.items():
            f.write(f'export {key}="{value}"\n')
        f.write("\n")
        
        # Export compiler-specific environment variables
        f.write("# Compiler environment variables\n")
        match COMPILER:
            case "gcc":
                gcc_path = f"/usr/bin/gcc-{TOOLCHAIN_VERSIONS['gcc']}"
                gxx_path = f"/usr/bin/g++-{TOOLCHAIN_VERSIONS['gcc']}"
                f.write(f'export CC="{gcc_path}"\n')
                f.write(f'export CXX="{gxx_path}"\n')
                # For GCC/Clang, SYSROOT points to custom-built glibc/libstdc++
                sysroot = TOOLCHAIN.sysroot_dir
                f.write(f'export SYSROOT="{sysroot}"\n')
            case "clang":
                clang_path = f"/usr/bin/clang-{TOOLCHAIN_VERSIONS['clang']}"
                clangxx_path = f"/usr/bin/clang++-{TOOLCHAIN_VERSIONS['clang']}"
                f.write(f'export CC="{clang_path}"\n')
                f.write(f'export CXX="{clangxx_path}"\n')
                # For GCC/Clang, SYSROOT points to custom-built glibc/libstdc++
                sysroot = TOOLCHAIN.sysroot_dir
                f.write(f'export SYSROOT="{sysroot}"\n')
            case "zig":
                zig_bin = os.path.join(TOOLCHAIN.zig.package_dir, 'zig')
                f.write(f'export CC="{zig_bin} cc"\n')
                f.write(f'export CXX="{zig_bin} c++"\n')
                # Zig uses its own bundled libc, no traditional sysroot
                f.write('# Note: Zig uses its own bundled libc\n')
            case _:
                raise ValueError(f"Unsupported compiler: {COMPILER}")
        f.write("\n")
        
        # Set internal variables for container entrypoint (not exported)
        f.write("# Internal variables for container entrypoint (not exported to user environment)\n")
        f.write(f'CLICE_WORKDIR="{CLICE_WORKDIR}"\n')
        f.write(f'RELEASE_PACKAGE_DIR="{RELEASE_PACKAGE_DIR}"\n')
        f.write('UV_PACKAGE_DIR_NAME="uv-packages"\n')
        f.write("\n")
        
        # Write container entrypoint script
        f.write("# ========================================================================\n")
        f.write("# Container Entrypoint Script - Auto Python Environment Setup\n")
        f.write("# ========================================================================\n")
        f.write(entrypoint_content)
        f.write("\n")
    
    print(f"✅ .bashrc created at {bashrc_path}")
    print(f"  📝 Exported variables: {len(DEVELOPMENT_SHELL_VARS)} from DEVELOPMENT_SHELL_VARS")
    for key in DEVELOPMENT_SHELL_VARS.keys():
        print(f"    • {key}")
    print(f"  📝 Compiler: {COMPILER}")
    print(f"    • CC and CXX configured for {COMPILER}")
    if COMPILER in ["gcc", "clang"]:
        print(f"    • SYSROOT={TOOLCHAIN.sysroot_dir}")
    print("  📝 Internal variables: CLICE_WORKDIR, RELEASE_PACKAGE_DIR, UV_PACKAGE_DIR_NAME")
    print("  📝 Container entrypoint script embedded")

# ========================================================================
# 📋 Manifest Creation Functions
# ========================================================================

def create_comprehensive_manifest() -> None:
    print("📋 Creating comprehensive release manifest based on ALL_COMPONENTS...")
    
    # Create base manifest structure
    manifest = {
        "release_info": {
            "created_at": os.stat(RELEASE_PACKAGE_DIR).st_ctime if os.path.exists(RELEASE_PACKAGE_DIR) else None,
            "stage": "final_release",
            "version": "1.0.0"
        },
        "components": {},
        "summary": {
            "total_components": 0,
            "available_components": 0,
            "total_files": 0,
            "total_size_mb": 0.0
        }
    }
    
    # Process each component from ALL_COMPONENTS
    for component in ALL_COMPONENTS:
        package_dir = component.package_dir
        
        component_info = {
            "name": component.name,
            "type": component.__class__.__name__,
            "version": getattr(component, 'version', 'unknown'),
            "file_count": 0,
            "size_mb": 0.0
        }
        
        # Calculate component statistics
        file_count = sum(len(files) for _, _, files in os.walk(package_dir))
        dir_size = sum(
            os.path.getsize(os.path.join(dirpath, filename))
            for dirpath, _, filenames in os.walk(package_dir)
            for filename in filenames
        ) / (1024 * 1024)  # Convert to MB
        
        component_info["file_count"] = file_count
        component_info["size_mb"] = round(dir_size, 2)
        
        # Component-specific details
        match component.name:
            case "apt":
                # Count APT packages
                apt_packages = []
                for file in os.listdir(package_dir):
                    if file.endswith('.deb'):
                        pkg_name = file.split('_')[0]
                        if pkg_name not in apt_packages:
                            apt_packages.append(pkg_name)
                component_info["packages"] = sorted(apt_packages)
                component_info["package_count"] = len(apt_packages)

            case "uv":
                # UV and Python version information
                component_info["uv_details"] = {
                    "uv_version": UV.version,
                    "python_version": UV.python_version,
                }

            case "toolchain":
                # Toolchain specific information
                component_info["toolchain_details"] = {
                    "glibc_version": TOOLCHAIN.glibc.version,
                    "gcc_version": TOOLCHAIN.gcc.version,
                    "linux_version": TOOLCHAIN.linux.version,
                    # "llvm_version": TOOLCHAIN.llvm.version,
                }
            
            case "clice-setup-scripts":
                # Setup scripts information - executed in-place
                component_info["note"] = "Executed in-place during expansion, not extracted to CLICE_WORKDIR"
                component_info["structure"] = "Complete directory tree (config/, docker/linux/utility/)"
            
            case "bashrc":
                # Bashrc information
                bashrc_file = os.path.join(package_dir, ".bashrc")
                component_info["bashrc_path"] = bashrc_file
                component_info["bashrc_size_kb"] = round(os.path.getsize(bashrc_file) / 1024, 2)
        
        manifest["components"][component.name] = component_info
        manifest["summary"]["total_components"] += 1
        manifest["summary"]["available_components"] += 1
        manifest["summary"]["total_files"] += component_info["file_count"]
        manifest["summary"]["total_size_mb"] += component_info["size_mb"]
    
    # Round summary size
    manifest["summary"]["total_size_mb"] = round(manifest["summary"]["total_size_mb"], 2)
    
    # Write manifest to release directory
    manifest_file = os.path.join(RELEASE_PACKAGE_DIR, "manifest.json")
    os.makedirs(RELEASE_PACKAGE_DIR, exist_ok=True)
    with open(manifest_file, 'w') as f:
        json.dump(manifest, f, indent=2)
    
    print(f"✅ Comprehensive manifest created: {manifest_file}")
    print(f"📊 Components: {manifest['summary']['available_components']}/{manifest['summary']['total_components']} available")
    print(f"📁 Total files: {manifest['summary']['total_files']}")
    print(f"📦 Total size: {manifest['summary']['total_size_mb']} MB")

def install_p7zip() -> None:
    print("📦 Installing p7zip for archive creation...")
    
    packages = " ".join(P7ZIP.build_prerequisites)
    run_command(f"apt install -y --no-install-recommends -o DPkg::Lock::Timeout=-1 {packages}")
    print("✅ p7zip installed successfully")

def create_final_release_package() -> None:
    """Create self-extracting 7z archive containing all components."""
    print("📦 Creating self-extracting release package with 7z SFX...")
    
    if not os.path.exists(RELEASE_PACKAGE_DIR):
        print("⚠️ No release package directory found")
        return
    
    # Ensure parent directory exists for the packed file
    packed_dir = os.path.dirname(PACKED_RELEASE_PACKAGE_PATH)
    os.makedirs(packed_dir, exist_ok=True)
    
    print(f"    📁 Source: {RELEASE_PACKAGE_DIR}")
    print(f"    📁 Target: {PACKED_RELEASE_PACKAGE_PATH}")
    
    
    # Create self-extracting archive using 7z with SFX module
    # The -sfx option creates a self-extracting executable
    print(f"🔧 Creating SFX archive with settings: {P7ZIP.compression_options}...")
    
    seven_zip_cmd = (
        f"7z a {P7ZIP.sfx_option} {" ".join(P7ZIP.compression_options)} "
        f"{PACKED_RELEASE_PACKAGE_PATH} "
        f"{RELEASE_PACKAGE_DIR}/*"
    )
    run_command(seven_zip_cmd)
    
    # Report package statistics
    package_size_mb = os.path.getsize(PACKED_RELEASE_PACKAGE_PATH) / (1024 * 1024)
    
    print(f"✅ Self-extracting release package created: {PACKED_RELEASE_PACKAGE_PATH}")
    print(f"📊 Package size: {package_size_mb:.1f} MB")
    print(f"ℹ️  Extract with: {PACKED_RELEASE_PACKAGE_PATH} -o<output_dir>")

# ========================================================================
# 🚀 Main Execution
# ========================================================================

def main() -> None:
    print("🚀 ========================================================================")
    print("🚀 CLICE RELEASE PACKAGE CREATOR - STAGE 3")
    print("🚀 ========================================================================")
    print("📦 Creating final release package from merged stage outputs")
    print("🚀 ========================================================================\n")
    
    # Define packaging jobs with proper dependency management
    setup_bashrc_job = Job("setup_bashrc", setup_environment_variables_and_entrypoint, ())
    install_p7zip_job = Job("install_p7zip", install_p7zip, ())
    create_manifest_job = Job("create_manifest", create_comprehensive_manifest, (), [setup_bashrc_job])
    create_package_job = Job("create_package", create_final_release_package, (), [create_manifest_job, install_p7zip_job])
    
    all_jobs = [
        setup_bashrc_job,
        install_p7zip_job,
        create_manifest_job,
        create_package_job,
    ]
    
    # Execute packaging tasks in parallel where possible
    scheduler = ParallelTaskScheduler(all_jobs)
    scheduler.run()
    
    print("\n🎉 ========================================================================")
    print("🎉 STAGE 3 COMPLETED SUCCESSFULLY!")
    print("🎉 ========================================================================")
    print(f"✅ Final release package: {PACKED_RELEASE_PACKAGE_PATH}")
    print(f"✅ Manifest: {RELEASE_PACKAGE_DIR}/manifest.json")
    print(f"✅ Bashrc: {BASHRC.bashrc_path}")
    print("🎉 ========================================================================")

if __name__ == "__main__":
    main()