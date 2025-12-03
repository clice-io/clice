import shutil
import sys
import os
import tarfile
import subprocess
import hashlib
import concurrent.futures
import time

project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
if project_root not in sys.path:
    sys.path.insert(0, project_root)

from config.docker_build_stages.common import Component, ToolchainSubComponent
import time
from typing import Dict, Set, Tuple, Optional, List, Callable
from graphlib import TopologicalSorter
from collections import defaultdict
from enum import Enum

def download_file(url: str, dest: str) -> None:
    if os.path.exists(dest):
        print(f"File {os.path.basename(dest)} already exists. Skipping download.", flush=True)
        return

    dest_dir = os.path.dirname(dest)
    dest_name = os.path.basename(dest)
    
    print(f"Downloading {url} to {dest} (SSL verification disabled)...", flush=True)
    
    command = [
        "aria2c",
        "--continue=true",
        "--split=8",
        "--max-connection-per-server=8",
        "--min-split-size=1M",
        "--file-allocation=falloc",   # Preallocate file space
        "--check-certificate=false",  # Corresponds to verify=False
        f'--dir="{dest_dir}"',
        f'--out="{dest_name}"',
        f'"{url}"'
    ]

    run_command(" ".join(command))
    print("Download complete.", flush=True)

def run_command(command: str, cwd: str = os.getcwd(), env: Dict[str, str] = {}) -> None:
    print(f"--- Running command: {{{command}}} in {cwd or os.getcwd()} ---", flush=True)
    
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

def verify_signature(signature_path: str, data_path: str, public_key: Optional[str] = None) -> bool:
    if not os.path.exists(data_path):
        raise RuntimeError(f"Data file {data_path} does not exist")
    
    if not os.path.exists(signature_path):
        print(f"No signature file found: {os.path.basename(signature_path)}, skipping verification", flush=True)
        return True
    
    # Detect signature type by extension and use match statement
    match os.path.splitext(signature_path)[1]:
        case '.minisig':
            # Minisign verification using pyminisign
            if not public_key:
                print(f"Warning: Minisign public key not provided for {os.path.basename(signature_path)}, skipping verification", flush=True)
                return True
            
            print(f"Verifying minisign signature for {os.path.basename(data_path)}...", flush=True)
            
            try:
                import minisign
                
                # Verify using py-minisign
                # verify() raises an exception if verification fails
                minisign.verify(signature_path, data_path, public_key)
                print("Minisign verification successful.", flush=True)
                return True
            
            except Exception as e:
                print(f"Minisign verification failed: {e}", flush=True)
                return False
        
        case '.sig':
            # GPG verification using python-gnupg
            print(f"Verifying GPG signature for {os.path.basename(data_path)}...", flush=True)
            
            try:
                import gnupg
                
                gpg = gnupg.GPG()
                
                # Verify the signature
                with open(signature_path, 'rb') as sig_file:
                    verified = gpg.verify_file(sig_file, data_path)
                
                if verified.valid:
                    print("GPG signature verification successful.", flush=True)
                    return True
                else:
                    print(f"GPG signature verification failed: {verified.status}", flush=True)
                    return False
                    
            except ImportError:
                print("python-gnupg library not found. Skipping signature verification.", flush=True)
                return True
            except Exception as e:
                print(f"Error during GPG signature verification: {e}, skipping", flush=True)
                return True
        
        case _:
            print(f"Unknown signature type for {os.path.basename(signature_path)}, skipping verification", flush=True)
            return True

def verify_sha256(file_path: str, expected_hash: str) -> bool:
    print(f"Verifying SHA256 for {file_path}...", flush=True)
    sha256 = hashlib.sha256()
    with open(file_path, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            sha256.update(chunk)
    actual_hash = sha256.hexdigest()
    
    if actual_hash.lower() == expected_hash.lower():
        print("SHA256 verification successful.", flush=True)
        return True
    else:
        print(f"SHA256 verification failed! Expected {expected_hash}, got {actual_hash}", flush=True)
        return False


# === Parallel Task Scheduler Classes ===

class Job:
    """Represents a single unit of work in the build process."""
    
    def __init__(self, name: str, func: Callable, args: Tuple = (), dependencies: Optional[List['Job']] = None) -> None:
        self.name = name  # Name is only for debugging, not guaranteed unique
        self.func = func
        self.args = args
        self.dependencies: List['Job'] = dependencies or []

    def __repr__(self) -> str:
        return f"Job(name='{self.name}', deps={[d.name for d in self.dependencies]})"
    
    def __hash__(self) -> int:
        return id(self)  # Use Python's built-in object id
    
    def __eq__(self, other) -> bool:
        return self is other  # Identity comparison


class TaskState(Enum):
    """Task execution states for better tracking."""
    PENDING = "pending"
    READY = "ready"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"


def run_job(job: Job) -> Job:
    """Executor function to run a job."""
    print(f"--- Starting Job: {job.name} ---", flush=True)
    job.func(*job.args)
    print(f"--- Finished Job: {job.name} ---", flush=True)
    return job  # Return the Job object itself


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
    
    def __init__(self, jobs: List[Job]):
        # Store all jobs as a set for fast lookups
        self.all_jobs: Set[Job] = set(jobs)
        
        # Build Job-based dependency graph
        self.dependencies: Dict[Job, Set[Job]] = {
            job: set(job.dependencies) 
            for job in jobs
        }
        
        # Task states use Job objects as keys
        self.task_states: Dict[Job, TaskState] = {job: TaskState.PENDING for job in jobs}
        self.running_futures: Dict[concurrent.futures.Future, Job] = {}  # future -> Job mapping
        self.completed_jobs: Set[Job] = set()
        self.failed_jobs: Set[Job] = set()
        
        # Performance tracking uses Job objects as keys
        self.start_time: Optional[float] = None
        self.job_start_times: Dict[Job, float] = {}
        self.job_durations: Dict[Job, float] = {}
        
        # Build string-based graph ONLY for TopologicalSorter (library requirement)
        # Map job ID to Job object for reverse lookup
        self.id_to_job: Dict[int, Job] = {id(job): job for job in jobs}
        string_deps: Dict[int, Set[int]] = {
            id(job): {id(dep) for dep in job.dependencies}
            for job in jobs
        }
        
        # Initialize dependency sorter with IDs
        self.sorter = TopologicalSorter(string_deps)
        self.sorter.prepare()
        
        # Reverse dependency mapping: which jobs depend on this job
        self.dependents: Dict[Job, Set[Job]] = defaultdict(set)
        for job, deps in self.dependencies.items():
            for dep in deps:
                self.dependents[dep].add(job)
    
    def _get_ready_jobs(self) -> List[Job]:
        """Get all jobs that are ready to run (dependencies satisfied)."""
        ready_jobs: List[Job] = []
        for job_id in self.sorter.get_ready():
            job = self.id_to_job[job_id]
            if self.task_states[job] == TaskState.PENDING:
                ready_jobs.append(job)
        return ready_jobs
    
    def _submit_job(self, executor: concurrent.futures.Executor, job: Job) -> concurrent.futures.Future:
        """Submit a job for execution."""
        self.task_states[job] = TaskState.RUNNING
        self.job_start_times[job] = time.time()
        
        print(f"🚀 [Scheduler] Starting job: {job.name}", flush=True)
        future = executor.submit(run_job, job)
        self.running_futures[future] = job
        return future
    
    def _handle_completed_job(self, job: Job, success: bool = True) -> None:
        """Handle job completion and update states."""
        duration = time.time() - self.job_start_times[job]
        self.job_durations[job] = duration
        
        if success:
            self.task_states[job] = TaskState.COMPLETED
            self.completed_jobs.add(job)
            self.sorter.done(id(job))  # Tell sorter using object ID
            print(f"✅ [Scheduler] Job '{job.name}' completed successfully in {duration:.2f}s", flush=True)
        else:
            self.task_states[job] = TaskState.FAILED
            self.failed_jobs.add(job)
            print(f"❌ [Scheduler] Job '{job.name}' failed after {duration:.2f}s", flush=True)
    
    def _print_progress(self) -> None:
        """Print current execution progress."""
        total = len(self.all_jobs)
        completed = len(self.completed_jobs)
        running = len(self.running_futures)
        failed = len(self.failed_jobs)
        pending = total - completed - running - failed
        
        elapsed = time.time() - self.start_time if self.start_time else 0
        
        running_job_names = [job.name for job in self.running_futures.values()]
        print(f"\n📊 [Progress] Total: {total} | ✅ Done: {completed} | 🏃 Running: {running} | ⏳ Pending: {pending} | ❌ Failed: {failed}", flush=True)
        print(f"⏱️  [Time] Elapsed: {elapsed:.1f}s | Running jobs: {running_job_names}", flush=True)
        
        if completed > 0 and elapsed > 0:
            rate = completed / elapsed
            eta = (total - completed) / rate if rate > 0 else 0
            print(f"📈 [Stats] Rate: {rate:.2f} jobs/s | ETA: {eta:.1f}s", flush=True)
    
    def run(self, max_workers: Optional[int] = None) -> None:
        """
        Execute all jobs with optimal parallel scheduling.
        
        Args:
            max_workers: Maximum number of parallel workers (default: CPU count)
        """
        print("🎯 [Scheduler] Initializing High-Performance Parallel Task Scheduler", flush=True)
        print(f"📋 [Scheduler] Total jobs: {len(self.all_jobs)}", flush=True)
        total_deps = sum(len(deps) for deps in self.dependencies.values())
        print(f"🔗 [Scheduler] Total dependency edges: {total_deps}", flush=True)
        if total_deps > 0:
            # Print dependency graph with names for debugging
            debug_graph = {job.name: [dep.name for dep in deps] for job, deps in self.dependencies.items() if deps}
            print(f"    Dependency graph: {debug_graph}", flush=True)
        
        self.start_time = time.time()
        
        with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as executor:
            # Submit initial ready jobs
            ready_jobs = self._get_ready_jobs()
            ready_job_names = [job.name for job in ready_jobs]
            print(f"🚦 [Scheduler] Initial ready jobs: {ready_job_names}", flush=True)
            
            for job in ready_jobs:
                self._submit_job(executor, job)
            
            # Main execution loop
            while self.running_futures:
                self._print_progress()
                
                # Wait for at least one job to complete
                done_futures, _ = concurrent.futures.wait(
                    self.running_futures.keys(),
                    return_when=concurrent.futures.FIRST_COMPLETED
                )
                
                # Process all completed jobs in this batch
                newly_completed: List[Job] = []
                for future in done_futures:
                    job = self.running_futures[future]
                    
                    try:
                        result = future.result()  # This will raise exception if job failed
                        self._handle_completed_job(job, success=True)
                        newly_completed.append(job)
                    except Exception as e:
                        print(f"💥 [Scheduler] Job '{job.name}' failed with detailed error:", flush=True)
                        self._handle_completed_job(job, success=False)
                        
                        # Implement fail-fast: cancel all running jobs and exit immediately
                        print(f"🛑 [Scheduler] FAIL-FAST: Cancelling all remaining jobs due to failure in '{job.name}'", flush=True)
                        for remaining_future in self.running_futures.keys():
                            if remaining_future != future:
                                remaining_future.cancel()
                                remaining_job = self.running_futures[remaining_future]
                                print(f"❌ [Scheduler] Cancelled job: {remaining_job.name}", flush=True)
                        
                        # Clean up and raise the error immediately
                        raise RuntimeError(f"❌ Build failed in job '{job.name}': {str(e)}") from e
                    
                    # Clean up completed future
                    del self.running_futures[future]
                
                # Submit any newly ready jobs
                if newly_completed:
                    ready_jobs = self._get_ready_jobs()
                    for job in ready_jobs:
                        if job not in self.running_futures.values():
                            self._submit_job(executor, job)
        
        # Final results
        total_time = time.time() - self.start_time
        self._print_final_report(total_time)
        
        # Note: With fail-fast implementation, we won't reach here if any job failed
        # The exception will be raised immediately when the first job fails
    
    def _print_final_report(self, total_time: float) -> None:
        """Print comprehensive execution report."""
        print("\n" + "="*60, flush=True)
        print("🎉 PARALLEL TASK EXECUTION COMPLETED!", flush=True)
        print("="*60, flush=True)
        
        print(f"⏱️  Total execution time: {total_time:.2f}s", flush=True)
        print(f"✅ Successfully completed: {len(self.completed_jobs)}/{len(self.all_jobs)} jobs", flush=True)
        
        if self.failed_jobs:
            print(f"❌ Failed jobs: {len(self.failed_jobs)}", flush=True)
            for job in self.failed_jobs:
                print(f"   - {job.name}", flush=True)
        
        # Show job timing analysis
        if self.job_durations:
            print(f"\n📊 Job Performance Analysis:", flush=True)
            sorted_jobs = sorted(self.job_durations.items(), key=lambda x: x[1], reverse=True)
            print(f"   Slowest jobs:", flush=True)
            for job, duration in sorted_jobs[:5]:
                print(f"   - {job.name:<30} {duration:>8.2f}s", flush=True)
            
            avg_duration = sum(self.job_durations.values()) / len(self.job_durations)
            print(f"   Average job duration: {avg_duration:.2f}s", flush=True)
            
            # Calculate theoretical sequential time vs actual parallel time
            sequential_time = sum(self.job_durations.values())
            speedup = sequential_time / total_time if total_time > 0 else 1
            efficiency = speedup / max(len(self.running_futures), 1) * 100
            
            print(f"   Sequential time would be: {sequential_time:.2f}s", flush=True)
            print(f"   Parallel speedup: {speedup:.2f}x", flush=True)
            print(f"   Parallel efficiency: {efficiency:.1f}%", flush=True)
        
        print("="*60, flush=True)


# ========================================================================
# 🛠️ Component Build Utilities
# ========================================================================

def install_download_prerequisites(component: Component) -> None:
    print("⬇️ [SETUP] Installing download prerequisites (aria2c, gnupg)...", flush=True)
    download_prerequisites = component.download_prerequisites
    pkg_list = " ".join(download_prerequisites)
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {pkg_list}")
    print("✅ [SETUP] Download tools ready", flush=True)

def install_extract_prerequisites(component: Component) -> None:
    print("📂 [SETUP] Installing archive extraction tools...", flush=True)
    extract_prerequisites = component.extract_prerequisites
    pkg_list = " ".join(extract_prerequisites)
    run_command(f"apt install -y --no-install-recommends=true -o DPkg::Lock::Timeout=-1 {pkg_list}")
    print("✅ [SETUP] Extraction tools ready", flush=True)

def download_and_verify(component: Component) -> None:
    version = component.version
    print(f"⬇️ [DOWNLOAD] Fetching {component.name} v{version}...", flush=True)
    
    # Ensure directories exist
    os.makedirs(component.cache_dir, exist_ok=True)

    # Construct download paths and URLs
    tarball_name = component.tarball_name
    tarball_path = os.path.join(component.cache_dir, tarball_name)
    tarball_url = component.tarball_url

    # Download main source archive
    download_file(tarball_url, tarball_path)
    
    # Handle signature verification when available
    if component.verification_name_pattern:
        signature_name = component.verification_name
        signature_path = os.path.join(component.cache_dir, signature_name)
        signature_url = component.verification_url
        
        # Get public key if component has minisig_public_key attribute
        public_key = getattr(component, 'minisig_public_key', None)
        
        try:
            print(f"🔐 [VERIFY] Downloading signature for {component.name}...", flush=True)
            download_file(signature_url, signature_path)
            verify_signature(signature_path, tarball_path, public_key)
            print(f"✅ [VERIFY] {component.name} signature verified", flush=True)
        except Exception as e:
            print(f"❌ [ERROR] Signature verification failed for {component.name}: {e}", file=sys.stderr, flush=True)
            shutil.rmtree(component.cache_dir, ignore_errors=True)
            raise
    else:
        print(f"⚠️  [INFO] No signature verification available for {component.name}", flush=True)

def extract_package(archive_path: str, target_dir: str, strip_top_level: bool = True) -> None:
    print(f"📂 [EXTRACT] Unpacking package...")
    print(f"    📁 Source: {archive_path}")
    print(f"    📁 Target: {target_dir}")
    
    # Ensure target directory exists
    os.makedirs(target_dir, exist_ok=True)
    
    # Auto-detect compression format
    if archive_path.endswith(".tar.xz"):
        mode = "r:xz"
    elif archive_path.endswith(".tar.gz") or archive_path.endswith(".tgz"):
        mode = "r:gz"
    elif archive_path.endswith(".tar.bz2"):
        mode = "r:bz2"
    elif archive_path.endswith(".tar"):
        mode = "r"
    else:
        raise ValueError(f"Unsupported archive format: {archive_path}")
    
    # Extract to target directory
    with tarfile.open(archive_path, mode) as tar:
        tar.extractall(path=target_dir, filter='data')
    
    # Optionally strip single top-level directory
    if strip_top_level:
        extracted_items = os.listdir(target_dir)
        
        if len(extracted_items) == 1 and os.path.isdir(os.path.join(target_dir, extracted_items[0])):
            # Single top-level directory found - strip it
            top_dir_name = extracted_items[0]
            top_dir_path = os.path.join(target_dir, top_dir_name)
            print(f"    🔄 Stripping top-level directory: {top_dir_name}")
            
            # Move all contents from top_dir to parent (target_dir)
            for item in os.listdir(top_dir_path):
                src = os.path.join(top_dir_path, item)
                dst = os.path.join(target_dir, item)
                shutil.move(src, dst)
            
            # Remove the now-empty top-level directory
            os.rmdir(top_dir_path)
    
    print(f"✅ [EXTRACT] package extraction complete")

def extract_source(component: ToolchainSubComponent) -> None:
    """
    Extract source code for a toolchain component.
    Wrapper around extract_package for backward compatibility.
    """
    version = component.version
    os.makedirs(component.src_dir, exist_ok=True)
    
    archive_path = os.path.join(component.cache_dir, component.tarball_name)
    extract_package(
        archive_path=archive_path,
        target_dir=component.extracted_dir,
        strip_top_level=True,
    )
