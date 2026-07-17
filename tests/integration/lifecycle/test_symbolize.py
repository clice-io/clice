"""End-to-end check of the release symbol-separation flow.

Replays the release pipeline on the freshly built binary (split DWARF, convert
to GSYM, strip), crashes a worker of the stripped binary, and verifies
scripts/symbolize.py recovers function/file information from the raw-address
crash log. This is the guarantee that shipped crash logs stay actionable.
"""

import asyncio
import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path

import pytest

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb
from tests.tools.checks import anomalies_in_log_messages
from tests.integration.lifecycle.test_anomaly import child_pids

REPO_ROOT = Path(__file__).resolve().parents[3]


def run_tool(*command: str | Path) -> None:
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    assert result.returncode == 0, f"{command[0]} failed: {result.stderr[:2000]}"


@pytest.mark.skipif(sys.platform != "linux", reason="worker discovery uses /proc")
@pytest.mark.allow_anomaly
@pytest.mark.timeout(600)
async def test_stripped_crash_symbolization(executable, tmp_path):
    if "Debug" in executable.parts:
        pytest.skip("release separation flow targets non-ASan builds")

    for tool in ("llvm-objcopy", "llvm-strip", "llvm-gsymutil"):
        assert shutil.which(tool), f"{tool} must be available for the symbol flow"

    # Replay the clice-strip / clice-pack-symbol steps from cmake/release.cmake.
    stripped = tmp_path / "clice"
    debug_file = tmp_path / "clice.debug"
    gsym_file = tmp_path / "clice.gsym"
    shutil.copy(executable, stripped)
    run_tool("llvm-objcopy", "--only-keep-debug", stripped, debug_file)
    run_tool(
        "llvm-gsymutil",
        "--convert",
        debug_file,
        "--merged-functions",
        "--quiet",
        "--out-file",
        gsym_file,
    )
    run_tool("llvm-strip", "--strip-debug", "--strip-unneeded", stripped)

    workspace = tmp_path / "ws"
    workspace.mkdir()
    (workspace / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(workspace, ["main.cpp"])

    # Force the raw-address dump: with in-process symbolization disabled the
    # log carries only "clice 0x..." frames, exactly like a user machine
    # without llvm-symbolizer.
    os.environ["LLVM_DISABLE_SYMBOLIZATION"] = "1"
    os.environ["CLICE_ANOMALY_NO_TRAP"] = "1"
    try:
        client = await make_client(stripped, workspace)
    finally:
        os.environ.pop("LLVM_DISABLE_SYMBOLIZATION", None)
        os.environ.pop("CLICE_ANOMALY_NO_TRAP", None)

    try:
        await client.open_and_wait(workspace / "main.cpp")

        workers = child_pids(client.server.pid)
        assert workers, "server should have spawned worker processes"
        # SIGABRT for the same reasons as test_anomaly: it exercises the crash
        # handler and is not intercepted by sanitizers.
        os.kill(workers[0], signal.SIGABRT)

        for _ in range(50):
            if "WorkerCrash" in anomalies_in_log_messages(client):
                break
            await asyncio.sleep(0.2)
    finally:
        await shutdown_client(client)

    logs_dir = workspace / ".clice" / "logs"
    crash_logs = [
        p
        for p in logs_dir.rglob("*.log")
        if p.name != "master.log"
        and "CRASH STACK TRACE" in p.read_text(errors="replace")
    ]
    assert crash_logs, "worker crash backtrace should be written to its log file"
    raw = crash_logs[0].read_text(errors="replace")
    assert "main executable base: 0x" in raw
    assert "logging.cpp" not in raw, "stripped binary must not self-symbolize"

    result = subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "scripts" / "symbolize.py"),
            str(crash_logs[0]),
            "--symbols",
            str(gsym_file),
        ],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, f"symbolize.py failed: {result.stderr[:2000]}"
    # The crash handler itself is always on the stack; recovering its source
    # file proves rebasing and GSYM lookup both worked.
    assert "logging.cpp" in result.stdout, (
        f"symbolized output should name the crash handler source:\n"
        f"{result.stdout[:3000]}"
    )
