#!/usr/bin/env python3
"""
📦 Clice Release Package Creator - Stage 3

This script handles the final packaging stage of the multi-stage Docker build.
It merges the outputs from Stage 1 (toolchain) and Stage 2 (dependencies),
creates a comprehensive manifest, and packages everything into a single
compressed archive for the release image.

Components Merged:
    • Custom compiler toolchain from Stage 1
    • Development dependencies from Stage 2
    • Combined dependency manifest
    • Final compressed release package

The script ensures all components from both stages are properly combined
and packaged for efficient Docker layer caching and distribution.
"""

import os
import sys
import tarfile
import json
import shutil

# Add project root to Python path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from config.build_config import (
    PACKED_RELEASE_PACKAGE_PATH,
    RELEASE_PACKAGE_DIR,
    CLICE_WORKDIR,
    DEVELOPMENT_SHELL_VARS,
    ALL_COMPONENTS,
    # Component instances for structured access
    TOOLCHAIN,
    CLICE_SETUP_SCRIPTS,
    BASHRC,
    UV
)

# Import build utilities for parallel execution
from build_utils import (
    Job,
    ParallelTaskScheduler
)

# ========================================================================
# 🌍 Environment Setup Functions
# ========================================================================

def setup_environment_variables_and_entrypoint():
    """
    Setup .bashrc with environment variables and container entrypoint script.
    
    This function creates a complete .bashrc file that:
    1. Exports environment variables from DEVELOPMENT_SHELL_VARS for persistent shell use
    2. Sets internal variables (CLICE_WORKDIR, etc.) without export for script-only use
    3. Embeds the container entrypoint script for auto Python environment setup
    """
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
    print("  📝 Internal variables: CLICE_WORKDIR, RELEASE_PACKAGE_DIR, UV_PACKAGE_DIR_NAME")
    print("  📝 Container entrypoint script embedded")

def copy_setup_scripts():
    """Copy setup scripts and configuration files as complete directory structure."""
    print("📋 Copying setup scripts and configuration files...")
    
    # Get files to copy from component definition
    for src_rel in CLICE_SETUP_SCRIPTS.files_to_copy:
        src = os.path.join(CLICE_WORKDIR, src_rel)
        dst = os.path.join(CLICE_SETUP_SCRIPTS.package_dir, src_rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f"  ✅ Copied: {src} -> {dst}")
    
    print(f"✅ Setup scripts and configs copied to {CLICE_SETUP_SCRIPTS.package_dir}")

# ========================================================================
# 📋 Manifest Creation Functions
# ========================================================================

def create_comprehensive_manifest():
    """
    📋 Create Comprehensive Release Manifest
    
    Creates a detailed manifest of all components based on ALL_COMPONENTS
    configuration. Analyzes actual package directories and creates a
    comprehensive overview of the release package contents.
    """
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
                    "llvm_version": TOOLCHAIN.llvm.version,
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

def create_final_release_package():
    """
    📦 Create Final Release Package
    
    Creates the final compressed archive containing all components from
    both build stages. Uses maximum XZ compression for minimal size.
    
    The package contains:
    - Custom compiler toolchain (Stage 1)
    - Development dependencies (Stage 2) 
    - Comprehensive release manifest
    - All directory structures preserved
    """
    print("📦 Creating final release package with maximum XZ compression...")
    
    if not os.path.exists(RELEASE_PACKAGE_DIR):
        print("⚠️ No release package directory found")
        return
    
    # Ensure parent directory exists for the packed file
    packed_dir = os.path.dirname(PACKED_RELEASE_PACKAGE_PATH)
    os.makedirs(packed_dir, exist_ok=True)
    
    # Create archive with maximum XZ compression
    print(f"    📁 Source: {RELEASE_PACKAGE_DIR}")
    print(f"    📁 Target: {PACKED_RELEASE_PACKAGE_PATH}")
    
    # LZMA could be optimized with multithreading, but reduces compress rate
    # With higher preset, multithreading benefits diminish. Ref: https://github.com/python/cpython/pull/114954
    # So we choose single-threaded for best compression
    with tarfile.open(PACKED_RELEASE_PACKAGE_PATH, 'w:xz', preset=9) as tar:
        # Add all subdirectories and files, preserving original directory structure
        for item in os.listdir(RELEASE_PACKAGE_DIR):
            item_path = os.path.join(RELEASE_PACKAGE_DIR, item)
            print(f"    📦 Adding: {item}")
            tar.add(item_path, arcname=item)
    
    # Report package statistics
    package_size_mb = os.path.getsize(PACKED_RELEASE_PACKAGE_PATH) / (1024 * 1024)
    
    # Calculate source directory size for compression ratio
    source_size_mb = sum(
        os.path.getsize(os.path.join(dirpath, filename))
        for dirpath, _, filenames in os.walk(RELEASE_PACKAGE_DIR)
        for filename in filenames
    ) / (1024 * 1024)
    
    compression_ratio = (source_size_mb - package_size_mb) / source_size_mb * 100 if source_size_mb > 0 else 0
    
    print(f"✅ Final release package created: {PACKED_RELEASE_PACKAGE_PATH}")
    print(f"📊 Source size: {source_size_mb:.1f} MB")
    print(f"📊 Package size: {package_size_mb:.1f} MB")
    print(f"📊 Compression ratio: {compression_ratio:.1f}%")

# ========================================================================
# 🚀 Main Execution
# ========================================================================

def main():
    """
    🚀 Main Stage 3 Execution
    
    Orchestrates the final packaging stage using parallel task execution:
    1. Setup .bashrc with environment variables and entrypoint script
    2. Copy setup scripts and configuration files
    3. Create comprehensive manifest based on ALL_COMPONENTS
    4. Package everything into final release archive
    
    This creates the complete release package ready for deployment.
    """
    print("🚀 ========================================================================")
    print("🚀 CLICE RELEASE PACKAGE CREATOR - STAGE 3")
    print("🚀 ========================================================================")
    print("📦 Creating final release package from merged stage outputs")
    print("🚀 ========================================================================\n")
    
    # Define packaging jobs with proper dependency management
    jobs = {
        "setup_bashrc": Job("setup_bashrc", setup_environment_variables_and_entrypoint, ()),
        "copy_setup_scripts": Job("copy_setup_scripts", copy_setup_scripts, ()),
        "create_manifest": Job("create_manifest", create_comprehensive_manifest, ()),
        "create_package": Job("create_package", create_final_release_package, ()),
    }
    
    # Define dependencies
    # - bashrc setup and script copy can run in parallel
    # - Manifest creation depends on bashrc and scripts being ready
    # - Package creation depends on manifest
    dependencies = {
        "setup_bashrc": set(),
        "copy_setup_scripts": set(),
        "create_manifest": {"setup_bashrc", "copy_setup_scripts"},
        "create_package": {"create_manifest"},
    }
    
    # Execute packaging tasks in parallel where possible
    scheduler = ParallelTaskScheduler(jobs, dependencies)
    scheduler.run()
    
    print("\n🎉 ========================================================================")
    print("🎉 STAGE 3 COMPLETED SUCCESSFULLY!")
    print("🎉 ========================================================================")
    print(f"✅ Final release package: {PACKED_RELEASE_PACKAGE_PATH}")
    print(f"✅ Manifest: {RELEASE_PACKAGE_DIR}/manifest.json")
    print(f"✅ Bashrc: {BASHRC.bashrc_path}")
    print(f"✅ Setup scripts: {CLICE_SETUP_SCRIPTS.package_dir}")
    print("🎉 ========================================================================")

if __name__ == "__main__":
    main()