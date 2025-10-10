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

# Add project root to Python path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from config.build_config import (
    PACKED_RELEASE_PACKAGE_PATH,
    RELEASE_PACKAGE_DIR,
    ALL_COMPONENTS,
    # Component instances for structured access
    TOOLCHAIN
)

# Import build utilities for parallel execution
from build_utils import (
    Job,
    ParallelTaskScheduler
)

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
        component_dir = os.path.join(RELEASE_PACKAGE_DIR, component.name)
        
        component_info = {
            "name": component.name,
            "type": component.__class__.__name__,
            "version": getattr(component, 'version', 'unknown'),
            "file_count": 0,
            "size_mb": 0.0
        }
        
        # Calculate component statistics
        file_count = sum(len(files) for _, _, files in os.walk(component_dir))
        dir_size = sum(
            os.path.getsize(os.path.join(dirpath, filename))
            for dirpath, _, filenames in os.walk(component_dir)
            for filename in filenames
        ) / (1024 * 1024)  # Convert to MB
        
        component_info["file_count"] = file_count
        component_info["size_mb"] = round(dir_size, 2)
        
        # Component-specific details
        match component.name:
            case "apt":
                # Count APT packages
                apt_packages = []
                for file in os.listdir(component_dir):
                    if file.endswith('.deb'):
                        pkg_name = file.split('_')[0]
                        if pkg_name not in apt_packages:
                            apt_packages.append(pkg_name)
                component_info["packages"] = sorted(apt_packages)
                component_info["package_count"] = len(apt_packages)

            case "python":
                # Count Python packages
                python_packages = []
                for file in os.listdir(component_dir):
                    if file.endswith('.whl'):
                        pkg_name = file.split('-')[0]
                        if pkg_name not in python_packages:
                            python_packages.append(pkg_name)
                component_info["packages"] = sorted(python_packages)
                component_info["package_count"] = len(python_packages)

            case "toolchain":
                # Toolchain specific information
                component_info["toolchain_details"] = {
                    "glibc_version": TOOLCHAIN.glibc.version,
                    "gcc_version": TOOLCHAIN.gcc.version,
                    "linux_version": TOOLCHAIN.linux.version,
                    "llvm_version": TOOLCHAIN.llvm.version,
                }
        
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
    
    try:
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
        
    except Exception as e:
        print(f"❌ Failed to create release package: {e}")
        raise

# ========================================================================
# 🚀 Main Execution
# ========================================================================

def main():
    """
    🚀 Main Stage 3 Execution
    
    Orchestrates the final packaging stage using parallel task execution:
    1. Verify stage outputs are present (Stage 1 & 2 already merged by Docker COPY)
    2. Create comprehensive manifest based on ALL_COMPONENTS
    3. Package everything into final release archive
    
    This creates the complete release package ready for deployment.
    """
    print("🚀 ========================================================================")
    print("🚀 CLICE RELEASE PACKAGE CREATOR - STAGE 3")
    print("🚀 ========================================================================")
    print("📦 Creating final release package from merged stage outputs")
    print("🚀 ========================================================================\n")
    
    # Define packaging jobs with proper dependency management
    jobs = {
        "create_manifest": Job("create_manifest", create_comprehensive_manifest, ()),
        "create_package": Job("create_package", create_final_release_package, ()),
    }
    
    # Define dependencies - package creation depends on manifest
    dependencies = {
        "create_manifest": set(),
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
    print("🎉 ========================================================================")

if __name__ == "__main__":
    main()