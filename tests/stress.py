#!/usr/bin/env python3
"""Stress-test clice on a real project (LLVM, Linux, etc.).

Usage:
    # Full indexing test on LLVM (wait for completion):
    pixi run python tests/stress.py /home/ykiko/workspace/llvm-project \
        --executable build/RelWithDebInfo/bin/clice

    # Time-limited run (just see how far it gets in 5 minutes):
    pixi run python tests/stress.py /home/ykiko/workspace/llvm-project \
        --executable build/RelWithDebInfo/bin/clice \
        --timeout 300

    # Custom worker limits:
    pixi run python tests/stress.py /home/ykiko/workspace/llvm-project \
        --executable build/RelWithDebInfo/bin/clice \
        --max-stateless 16
"""

import argparse
import asyncio
import json
import re
import signal
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
from tests.integration.utils.client import CliceClient


def parse_args():
    p = argparse.ArgumentParser(description="Stress-test clice on a real project")
    p.add_argument("project", help="Path to the project root")
    p.add_argument(
        "--executable",
        default="build/RelWithDebInfo/bin/clice",
        help="Path to clice binary",
    )
    p.add_argument(
        "--timeout",
        type=int,
        default=0,
        help="Max seconds to run (0 = wait for indexing to finish)",
    )
    p.add_argument(
        "--poll-interval",
        type=int,
        default=30,
        help="Seconds between log checks (default: 30)",
    )
    p.add_argument(
        "--max-stateless",
        type=int,
        default=0,
        help="Max stateless workers (0 = auto = CPU cores)",
    )
    p.add_argument(
        "--min-stateless",
        type=int,
        default=0,
        help="Min stateless workers (0 = default)",
    )
    p.add_argument(
        "--log-dir", default="", help="Log directory (default: auto in /tmp)"
    )
    return p.parse_args()


class LogAnalyzer:
    """Parse clice master log and extract indexing/scaling events."""

    # Patterns
    RE_INDEXING = re.compile(r"\[(\d+)/(\d+)\] Indexing (.+)")
    RE_INDEXED = re.compile(r"\[(\d+)/(\d+)\] Indexed (.+)")
    RE_INDEX_FAILED = re.compile(
        r"\[(\d+)/(\d+)\] Index (?:failed|IPC error|returned empty)"
    )
    RE_SCALE_UP = re.compile(r"Scaled up: spawned (\S+) \(alive=(\d+)\)")
    RE_RETIRE = re.compile(r"Retiring worker (\S+)")
    RE_RETIRED = re.compile(r"Worker (\S+) retired gracefully")
    RE_POOL_STARTED = re.compile(r"WorkerPool started: (\d+) stateless, (\d+) stateful")
    RE_LOW_LIMIT = re.compile(r"low_limit -> (\d+)")
    RE_CANCEL = re.compile(r"Cancelled (\d+) low-priority requests")
    RE_MEMORY = re.compile(
        r"Memory: ([\d.]+)% available.*low_limit=(\d+)/(\d+).*busy=(\d+).*alive=(\d+).*sat=(\d+)/idle=(\d+)"
    )
    RE_BG_INDEX_START = re.compile(r"Background indexing: starting, (\d+) files queued")
    RE_BG_INDEX_DONE = re.compile(r"Background indexing: queue exhausted")
    RE_CRASH = re.compile(r"Worker (\S+) (?:killed by signal|exited with code)")

    def __init__(self):
        self.total_files = 0
        self.indexed_count = 0
        self.failed_count = 0
        self.scale_up_events = []
        self.scale_down_events = []
        self.crashes = []
        self.cancellations = []
        self.pool_started = False
        self.initial_stateless = 0
        self.initial_stateful = 0
        self.peak_alive = 0
        self.indexing_complete = False
        self.last_memory_pct = 0.0
        self.last_alive = 0
        self.last_busy = 0
        self.lines_parsed = 0

    def parse_file(self, path: Path):
        if not path.exists():
            return
        with open(path) as f:
            for line in f:
                self.lines_parsed += 1
                self._parse_line(line)

    def parse_incremental(self, path: Path, offset: int) -> int:
        if not path.exists():
            return offset
        with open(path) as f:
            f.seek(offset)
            for line in f:
                self.lines_parsed += 1
                self._parse_line(line)
            return f.tell()

    def _parse_line(self, line: str):
        if m := self.RE_BG_INDEX_START.search(line):
            self.total_files = max(self.total_files, int(m.group(1)))

        if m := self.RE_INDEXED.search(line):
            cur, total = int(m.group(1)), int(m.group(2))
            self.indexed_count = max(self.indexed_count, cur)
            self.total_files = max(self.total_files, total)

        if m := self.RE_INDEX_FAILED.search(line):
            self.failed_count += 1

        if m := self.RE_INDEXING.search(line):
            self.total_files = max(self.total_files, int(m.group(2)))

        if self.RE_BG_INDEX_DONE.search(line):
            self.indexing_complete = True

        if m := self.RE_POOL_STARTED.search(line):
            self.pool_started = True
            self.initial_stateless = int(m.group(1))
            self.initial_stateful = int(m.group(2))
            self.peak_alive = self.initial_stateless

        if m := self.RE_SCALE_UP.search(line):
            name, alive = m.group(1), int(m.group(2))
            self.scale_up_events.append(name)
            self.last_alive = alive
            self.peak_alive = max(self.peak_alive, alive)

        if m := self.RE_RETIRE.search(line):
            self.scale_down_events.append(m.group(1))

        if m := self.RE_CRASH.search(line):
            self.crashes.append(m.group(1))

        if m := self.RE_CANCEL.search(line):
            self.cancellations.append(int(m.group(1)))

        if m := self.RE_MEMORY.search(line):
            self.last_memory_pct = float(m.group(1))
            self.last_alive = int(m.group(5))
            self.last_busy = int(m.group(4))
            self.peak_alive = max(self.peak_alive, self.last_alive)

    def progress_str(self) -> str:
        pct = (
            (self.indexed_count / self.total_files * 100) if self.total_files > 0 else 0
        )
        status = "DONE" if self.indexing_complete else "indexing"
        return (
            f"[{status}] {self.indexed_count}/{self.total_files} ({pct:.1f}%) | "
            f"alive={self.last_alive} busy={self.last_busy} peak={self.peak_alive} | "
            f"mem={self.last_memory_pct:.0f}% | "
            f"scale_up={len(self.scale_up_events)} scale_down={len(self.scale_down_events)} "
            f"crashes={len(self.crashes)}"
        )

    def summary(self, elapsed: float) -> str:
        lines = [
            "=" * 70,
            "STRESS TEST SUMMARY",
            "=" * 70,
            f"Duration:           {elapsed:.0f}s ({elapsed / 60:.1f} min)",
            f"Total files:        {self.total_files}",
            f"Indexed:            {self.indexed_count}",
            f"Failed:             {self.failed_count}",
            f"Indexing complete:  {'YES' if self.indexing_complete else 'NO'}",
            "",
            "Worker Scaling:",
            f"  Initial workers:  {self.initial_stateless} stateless, {self.initial_stateful} stateful",
            f"  Peak workers:     {self.peak_alive}",
            f"  Scale-up events:  {len(self.scale_up_events)}",
            f"  Scale-down events:{len(self.scale_down_events)}",
            f"  Crashes:          {len(self.crashes)}",
            f"  Cancellations:    {len(self.cancellations)}",
        ]
        if self.scale_up_events:
            lines.append(f"  Scaled up:        {', '.join(self.scale_up_events)}")
        if self.crashes:
            lines.append(f"  Crashed workers:  {', '.join(self.crashes)}")
        if self.indexed_count > 0 and elapsed > 0:
            rate = self.indexed_count / elapsed
            lines.append(f"\nIndexing rate:       {rate:.1f} files/s")
            if self.total_files > 0 and not self.indexing_complete:
                remaining = self.total_files - self.indexed_count
                eta = remaining / rate if rate > 0 else float("inf")
                lines.append(f"Estimated remaining:{eta:.0f}s ({eta / 60:.1f} min)")
        lines.append("=" * 70)
        return "\n".join(lines)


def find_master_log(log_dir: Path) -> Path | None:
    """Find the master log file in the log directory."""
    if not log_dir.exists():
        return None
    for session_dir in sorted(log_dir.iterdir(), reverse=True):
        if session_dir.is_dir():
            master_log = session_dir / "master.log"
            if master_log.exists():
                return master_log
    return None


async def run_stress_test(args):
    project = Path(args.project).resolve()
    executable = Path(args.executable).resolve()

    if not project.exists():
        print(f"ERROR: Project directory not found: {project}", file=sys.stderr)
        return 1
    if not executable.exists():
        print(f"ERROR: Executable not found: {executable}", file=sys.stderr)
        return 1

    if args.log_dir:
        log_dir = Path(args.log_dir)
    else:
        log_dir = Path(tempfile.mkdtemp(prefix="clice-stress-"))

    log_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = project / ".clice"
    cache_dir.mkdir(parents=True, exist_ok=True)

    print(f"Project:    {project}")
    print(f"Executable: {executable}")
    print(f"Log dir:    {log_dir}")
    print(
        f"Timeout:    {args.timeout}s"
        if args.timeout
        else "Timeout:    none (wait for completion)"
    )
    print()

    init_options = {
        "project": {
            "cache_dir": str(cache_dir),
            "logging_dir": str(log_dir),
            "enable_indexing": True,
        }
    }
    if args.max_stateless > 0:
        init_options["project"]["max_stateless_worker_count"] = args.max_stateless
    if args.min_stateless > 0:
        init_options["project"]["min_stateless_worker_count"] = args.min_stateless

    client = CliceClient()
    await client.start_io(str(executable), "server")

    server = getattr(client, "_server", None)
    start_time = time.monotonic()

    try:
        await client.initialize(project, initialization_options=init_options)
        print("Server initialized, background indexing should start...")
        print()

        analyzer = LogAnalyzer()
        log_offset = 0
        poll_count = 0

        while True:
            await asyncio.sleep(args.poll_interval)
            elapsed = time.monotonic() - start_time

            # Check if server died
            if server and server.returncode is not None:
                print(
                    f"\n!!! SERVER EXITED with code {server.returncode} after {elapsed:.0f}s !!!"
                )
                break

            # Find and parse log
            master_log = find_master_log(log_dir)
            if master_log:
                log_offset = analyzer.parse_incremental(master_log, log_offset)

            poll_count += 1
            print(f"[{elapsed:6.0f}s] {analyzer.progress_str()}")

            if analyzer.indexing_complete:
                print("\nIndexing complete!")
                break

            if args.timeout > 0 and elapsed >= args.timeout:
                print(f"\nTimeout reached ({args.timeout}s)")
                break

    except KeyboardInterrupt:
        elapsed = time.monotonic() - start_time
        print(f"\nInterrupted after {elapsed:.0f}s")
    except Exception as e:
        elapsed = time.monotonic() - start_time
        print(f"\nError after {elapsed:.0f}s: {e}")
    finally:
        elapsed = time.monotonic() - start_time

        # Parse final state of logs
        master_log = find_master_log(log_dir)
        if master_log:
            analyzer = LogAnalyzer()
            analyzer.parse_file(master_log)

        print()
        print(analyzer.summary(elapsed))

        # Graceful shutdown
        print("\nShutting down server...")
        try:
            await asyncio.wait_for(client.shutdown_async(None), timeout=10.0)
        except Exception:
            pass
        try:
            client.exit(None)
        except Exception:
            pass
        if server and server.returncode is None:
            try:
                await asyncio.wait_for(server.wait(), timeout=15.0)
            except asyncio.TimeoutError:
                server.kill()
                await server.wait()

        print(f"Log files: {log_dir}")
        if master_log:
            print(f"Master log: {master_log}")

    return 0 if not analyzer.crashes else 1


def main():
    args = parse_args()
    rc = asyncio.run(run_stress_test(args))
    sys.exit(rc)


if __name__ == "__main__":
    main()
