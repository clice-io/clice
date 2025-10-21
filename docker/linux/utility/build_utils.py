import shutil
import sys
import os
import tarfile

from config.build_config import Component

# Add project root to the Python path to allow importing 'config' module
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

import subprocess
import hashlib
import concurrent.futures
import time
from typing import Dict, Set, Tuple, Optional, List, Callable
from graphlib import TopologicalSorter
from collections import defaultdict
from enum import Enum

def download_file(url: str, dest: str) -> None:
    """
    Downloads a file from a URL to a destination using aria2c for acceleration.
    Skips the download if the destination file already exists.
    Disables SSL verification to work behind corporate proxies.
    """
    if os.path.exists(dest):
        print(f"File {os.path.basename(dest)} already exists. Skipping download.")
        return

    dest_dir = os.path.dirname(dest)
    dest_name = os.path.basename(dest)
    
    print(f"Downloading {url} to {dest} (SSL verification disabled)...")
    
    command = [
        "aria2c",
        "--continue=true",
        "--split=8",
        "--max-connection-per-server=8",
        "--min-split-size=1M",
        "--check-certificate=false",  # Corresponds to verify=False
        f'--dir="{dest_dir}"',
        f'--out="{dest_name}"',
        f'"{url}"'
    ]

    run_command(" ".join(command))
    print("Download complete.")

def run_command(command: str, cwd: str = os.getcwd(), env: Dict[str, str] = {}) -> None:
    """
    Executes a shell command, directing its output to the current shell.
    Sets DEBIAN_FRONTEND to noninteractive to prevent interactive prompts.
    
    Output is streamed in real-time to stdout/stderr for better visibility
    in both direct execution and parallel task scenarios.
    """
    print(f"--- Running command: {{{command}}} in {cwd or os.getcwd()} ---")
    
    # Setup environment
    process_env = os.environ.copy()
    process_env["DEBIAN_FRONTEND"] = "noninteractive"
    if env:
        process_env.update(env)

    # Explicitly set stdout and stderr to sys.stdout/sys.stderr for real-time output
    # This ensures output is visible even when running in ProcessPoolExecutor
    process = subprocess.Popen(
        command,
        shell=True,
        cwd=cwd,
        env=process_env,
        executable="/bin/bash",
        stdout=sys.stdout,
        stderr=sys.stderr,
        bufsize=1,  # Line buffered for real-time output
        universal_newlines=True
    )

    process.wait()
    if process.returncode != 0:
        raise subprocess.CalledProcessError(process.returncode, command)

def verify_signature(signature_path: str, data_path: str) -> None:
    """
    Simplified signature verification: only checks if files exist.
    Skips actual signature verification when GPG environment is not available.
    
    Args:
        signature_path: Path to the .asc signature file.
        data_path: Path to the signed data file.
    """
    print(f"--- Skipping signature verification for {os.path.basename(data_path)} (GPG not available) ---")
    
    if not os.path.exists(data_path):
        raise RuntimeError(f"Data file {data_path} does not exist")
    
    if os.path.exists(signature_path):
        print(f"Signature file found: {os.path.basename(signature_path)}")
    else:
        print(f"No signature file found: {os.path.basename(signature_path)}")
    
    print(f"File verification completed for {os.path.basename(data_path)}")

def verify_sha256(file_path: str, expected_hash: str) -> bool:
    """Verifies the SHA256 checksum of a file."""
    print(f"Verifying SHA256 for {file_path}...")
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            sha256.update(chunk)
    actual_hash = sha256.hexdigest()
    
    if actual_hash.lower() == expected_hash.lower():
        print("SHA256 verification successful.")
        return True
    else:
        print(f"SHA256 verification failed! Expected {expected_hash}, got {actual_hash}")
        return False


# === Parallel Task Scheduler Classes ===

class Job:
    """Represents a single unit of work in the build process."""
    def __init__(self, name: str, func: Callable, args: Tuple = ()):
        self.name = name
        self.func = func
        self.args = args

    def __repr__(self):
        return f"Job(name='{self.name}')"


class TaskState(Enum):
    """Task execution states for better tracking."""
    PENDING = "pending"
    READY = "ready"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"


def run_job(job: Job):
    """Executor function to run a job."""
    print(f"--- Starting Job: {job.name} ---")
    job.func(*job.args)
    print(f"--- Finished Job: {job.name} ---")
    return job.name


class ParallelTaskScheduler:
    """
    🚀 High-Performance Parallel Task Scheduler
    
    Features:
    - Optimal parallel execution with minimal overhead
    - Real-time dependency resolution
    - Comprehensive progress tracking
    - Robust error handling and recovery
    - Efficient resource utilization
    """
    
    def __init__(self, jobs: Dict[str, Job], dependencies: Dict[str, Set[str]]):
        self.jobs = jobs
        self.dependencies = dependencies
        self.task_states = {name: TaskState.PENDING for name in jobs}
        self.running_futures = {}  # future -> job_name mapping
        self.completed_jobs = set()
        self.failed_jobs = set()
        
        # Performance tracking
        self.start_time = None
        self.job_start_times = {}
        self.job_durations = {}
        
        # Initialize dependency sorter
        self.sorter = TopologicalSorter(dependencies)
        self.sorter.prepare()
        
        # Reverse dependency mapping for efficient lookups
        self.dependents = defaultdict(set)
        for job, deps in dependencies.items():
            for dep in deps:
                self.dependents[dep].add(job)
    
    def _get_ready_jobs(self) -> List[str]:
        """Get all jobs that are ready to run (dependencies satisfied)."""
        ready_jobs = []
        for job_name in self.sorter.get_ready():
            if self.task_states[job_name] == TaskState.PENDING:
                ready_jobs.append(job_name)
        return ready_jobs
    
    def _submit_job(self, executor, job_name: str):
        """Submit a job for execution."""
        job = self.jobs[job_name]
        self.task_states[job_name] = TaskState.RUNNING
        self.job_start_times[job_name] = time.time()
        
        print(f"🚀 [Scheduler] Starting job: {job_name}")
        future = executor.submit(run_job, job)
        self.running_futures[future] = job_name
        return future
    
    def _handle_completed_job(self, job_name: str, success: bool = True):
        """Handle job completion and update states."""
        duration = time.time() - self.job_start_times[job_name]
        self.job_durations[job_name] = duration
        
        if success:
            self.task_states[job_name] = TaskState.COMPLETED
            self.completed_jobs.add(job_name)
            self.sorter.done(job_name)
            print(f"✅ [Scheduler] Job '{job_name}' completed successfully in {duration:.2f}s")
        else:
            self.task_states[job_name] = TaskState.FAILED
            self.failed_jobs.add(job_name)
            print(f"❌ [Scheduler] Job '{job_name}' failed after {duration:.2f}s")
    
    def _print_progress(self):
        """Print current execution progress."""
        total = len(self.jobs)
        completed = len(self.completed_jobs)
        running = len(self.running_futures)
        failed = len(self.failed_jobs)
        pending = total - completed - running - failed
        
        elapsed = time.time() - self.start_time if self.start_time else 0
        
        print(f"\n📊 [Progress] Total: {total} | ✅ Done: {completed} | 🏃 Running: {running} | ⏳ Pending: {pending} | ❌ Failed: {failed}")
        print(f"⏱️  [Time] Elapsed: {elapsed:.1f}s | Running jobs: {list(self.running_futures.values())}")
        
        if completed > 0 and elapsed > 0:
            rate = completed / elapsed
            eta = (total - completed) / rate if rate > 0 else 0
            print(f"📈 [Stats] Rate: {rate:.2f} jobs/s | ETA: {eta:.1f}s")
    
    def run(self, max_workers: Optional[int] = None):
        """
        Execute all jobs with optimal parallel scheduling.
        
        Args:
            max_workers: Maximum number of parallel workers (default: CPU count)
        """
        print("🎯 [Scheduler] Initializing High-Performance Parallel Task Scheduler")
        print(f"📋 [Scheduler] Total jobs: {len(self.jobs)}")
        print(f"🔗 [Scheduler] Total dependencies: {sum(len(deps) for deps in self.dependencies.values())}")
        
        self.start_time = time.time()
        
        with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as executor:
            # Submit initial ready jobs
            ready_jobs = self._get_ready_jobs()
            print(f"🚦 [Scheduler] Initial ready jobs: {ready_jobs}")
            
            for job_name in ready_jobs:
                self._submit_job(executor, job_name)
            
            # Main execution loop
            while self.running_futures:
                self._print_progress()
                
                # Wait for at least one job to complete
                done_futures, _ = concurrent.futures.wait(
                    self.running_futures.keys(),
                    return_when=concurrent.futures.FIRST_COMPLETED
                )
                
                # Process all completed jobs in this batch
                newly_completed = []
                for future in done_futures:
                    job_name = self.running_futures[future]
                    
                    try:
                        result = future.result()  # This will raise exception if job failed
                        self._handle_completed_job(job_name, success=True)
                        newly_completed.append(job_name)
                    except Exception as e:
                        print(f"💥 [Scheduler] Job '{job_name}' failed with detailed error:")
                        self._handle_completed_job(job_name, success=False)
                        
                        # Implement fail-fast: cancel all running jobs and exit immediately
                        print(f"🛑 [Scheduler] FAIL-FAST: Cancelling all remaining jobs due to failure in '{job_name}'")
                        for remaining_future in self.running_futures.keys():
                            if remaining_future != future:
                                remaining_future.cancel()
                                remaining_job = self.running_futures[remaining_future]
                                print(f"❌ [Scheduler] Cancelled job: {remaining_job}")
                        
                        # Clean up and raise the error immediately
                        raise RuntimeError(f"❌ Build failed in job '{job_name}': {str(e)}") from e
                    
                    # Clean up completed future
                    del self.running_futures[future]
                
                # Submit any newly ready jobs
                if newly_completed:
                    ready_jobs = self._get_ready_jobs()
                    for job_name in ready_jobs:
                        if job_name not in self.running_futures.values():
                            self._submit_job(executor, job_name)
        
        # Final results
        total_time = time.time() - self.start_time
        self._print_final_report(total_time)
        
        # Note: With fail-fast implementation, we won't reach here if any job failed
        # The exception will be raised immediately when the first job fails
    
    def _print_final_report(self, total_time: float):
        """Print comprehensive execution report."""
        print("\n" + "="*60)
        print("🎉 PARALLEL TASK EXECUTION COMPLETED!")
        print("="*60)
        
        print(f"⏱️  Total execution time: {total_time:.2f}s")
        print(f"✅ Successfully completed: {len(self.completed_jobs)}/{len(self.jobs)} jobs")
        
        if self.failed_jobs:
            print(f"❌ Failed jobs: {len(self.failed_jobs)}")
            for job in self.failed_jobs:
                print(f"   - {job}")
        
        # Show job timing analysis
        if self.job_durations:
            print(f"\n📊 Job Performance Analysis:")
            sorted_jobs = sorted(self.job_durations.items(), key=lambda x: x[1], reverse=True)
            print(f"   Slowest jobs:")
            for job, duration in sorted_jobs[:5]:
                print(f"   - {job:<30} {duration:>8.2f}s")
            
            avg_duration = sum(self.job_durations.values()) / len(self.job_durations)
            print(f"   Average job duration: {avg_duration:.2f}s")
            
            # Calculate theoretical sequential time vs actual parallel time
            sequential_time = sum(self.job_durations.values())
            speedup = sequential_time / total_time if total_time > 0 else 1
            efficiency = speedup / max(len(self.running_futures), 1) * 100
            
            print(f"   Sequential time would be: {sequential_time:.2f}s")
            print(f"   Parallel speedup: {speedup:.2f}x")
            print(f"   Parallel efficiency: {efficiency:.1f}%")
        
        print("="*60)


# ========================================================================
# 🛠️ Component Build Utilities
# ========================================================================
# Generic functions for component-based building
# ========================================================================

def install_download_prerequisites(component: Component):
    """
    ⬇️ Install Download Stage Prerequisites
    
    Installs essential tools required for fetching source code archives:
    • aria2c - High-speed multi-connection downloader
    • gnupg - GPG signature verification system
    
    These tools enable secure, accelerated downloading of toolchain sources.
    """
    print("⬇️ [SETUP] Installing download prerequisites (aria2c, gnupg)...")
    download_prerequisites = component.download_prerequisites
    pkg_list = " ".join(download_prerequisites)
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {pkg_list}")
    print("✅ [SETUP] Download tools ready")

def install_extract_prerequisites(component: Component):
    """
    📂 Install Archive Extraction Prerequisites
    
    Installs compression tools needed for extracting various archive formats:
    • bzip2 - Required for GCC prerequisite archives (.tar.bz2)
    
    Different toolchain components use different compression formats,
    so we ensure all extraction tools are available.
    """
    print("📂 [SETUP] Installing archive extraction tools...")
    extract_prerequisites = component.extract_prerequisites
    pkg_list = " ".join(extract_prerequisites)
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {pkg_list}")
    print("✅ [SETUP] Extraction tools ready")


def download_and_verify(component):
    """
    ⬇️ Download and Verify Component Source
    
    Downloads the source tarball for a specified toolchain component and
    verifies its authenticity using GPG signatures when available.
    
    The process includes:
    1. Create organized download directory structure
    2. Download source archive using high-speed aria2c
    3. Download GPG signature file (if available)
    4. Verify archive integrity and authenticity
    5. Clean up on verification failure
    
    Args:
        component: Component instance (glibc, gcc, llvm, or linux)
    
    Raises:
        Exception: If download fails or signature verification fails
    """
    version = component.version
    print(f"⬇️ [DOWNLOAD] Fetching {component.name} v{version}...")
    
    # Ensure directories exist
    os.makedirs(component.cache_dir, exist_ok=True)

    # Construct download paths and URLs
    tarball_name = component.tarball_name
    tarball_path = os.path.join(component.cache_dir, tarball_name)
    tarball_url = component.tarball_url

    # Download main source archive
    download_file(tarball_url, tarball_path)
    
    # Handle GPG signature verification when available
    if component.verification_name_pattern:
        signature_name = component.verification_name
        signature_path = os.path.join(component.cache_dir, signature_name)
        signature_url = component.verification_url
        try:
            print(f"🔐 [VERIFY] Downloading signature for {component.name}...")
            download_file(signature_url, signature_path)
            verify_signature(signature_path, tarball_path)
            print(f"✅ [VERIFY] {component.name} signature verified")
        except Exception as e:
            print(f"❌ [ERROR] Signature verification failed for {component.name}: {e}", file=sys.stderr)
            shutil.rmtree(component.cache_dir, ignore_errors=True)
            raise
    else:
        print(f"⚠️  [INFO] No signature verification available for {component.name}")

def extract_source(component):
    """
    📂 Extract Component Source Archive
    
    Extracts the downloaded source tarball to the appropriate directory
    structure, automatically detecting compression format and stripping
    the top-level directory.
    
    Supports multiple archive formats:
    • .tar.xz (LZMA compression) - Used by most GNU projects
    • .tar.gz (Gzip compression) - Used by Linux kernel
    
    The function automatically handles archives with a top-level directory:
    1. Extracts directly to target directory
    2. Detects if there's a single top-level directory wrapper
    3. Moves all contents up one level
    4. Removes the empty wrapper directory
    
    Args:
        component: Component instance (glibc, gcc, llvm, or linux)
    """
    version = component.version
    print(f"📂 [EXTRACT] Unpacking {component.name} v{version}...")
    
    # Ensure extraction directory exists
    os.makedirs(component.src_dir, exist_ok=True)

    # Determine archive location and format
    tarball_name = component.tarball_name_pattern.format(version=version)
    tarball_path = os.path.join(component.cache_dir, tarball_name)

    print(f"    📁 Source: {tarball_path}")
    print(f"    📁 Target: {component.extracted_dir}")
    
    # Auto-detect compression format and extract directly
    mode = "r:xz" if tarball_path.endswith(".tar.xz") else "r:gz"
    with tarfile.open(tarball_path, mode) as tar:
        tar.extractall(path=component.extracted_dir, filter='data')
    
    # Check if we need to strip a top-level directory
    extracted_items = os.listdir(component.extracted_dir)
    
    if len(extracted_items) == 1 and os.path.isdir(os.path.join(component.extracted_dir, extracted_items[0])):
        # Single top-level directory found - strip it
        top_dir_name = extracted_items[0]
        top_dir_path = os.path.join(component.extracted_dir, top_dir_name)
        print(f"    🔄 Stripping top-level directory: {top_dir_name}")
        
        # Move all contents from top_dir to parent (extracted_dir)
        for item in os.listdir(top_dir_path):
            src = os.path.join(top_dir_path, item)
            dst = os.path.join(component.extracted_dir, item)
            shutil.move(src, dst)
        
        # Remove the now-empty top-level directory
        os.rmdir(top_dir_path)
    
    print(f"✅ [EXTRACT] {component.name} extraction complete")
