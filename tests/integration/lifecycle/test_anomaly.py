"""End-to-end check of the anomaly reporting machinery.

Kills a worker process and verifies the master reports `[anomaly:WorkerCrash]`
both via window/logMessage and in its log file — the same channels
assert_no_anomaly() watches in every other test's teardown.
"""

import asyncio
import os
import signal
import sys
from pathlib import Path

import pytest

from tests.conftest import make_client, shutdown_client
from tests.integration.utils import write_cdb
from tests.integration.utils.assertions import (
    anomalies_in_log_files,
    anomalies_in_log_messages,
)


def child_pids(parent_pid: int) -> list[int]:
    pids = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
        except OSError:
            continue
        # /proc/<pid>/stat: pid (comm) state ppid ...
        ppid = int(stat.rsplit(")", 1)[1].split()[1])
        if ppid == parent_pid:
            pids.append(int(entry.name))
    return pids


@pytest.mark.skipif(sys.platform != "linux", reason="worker discovery uses /proc")
@pytest.mark.allow_anomaly
async def test_worker_crash_reported(executable, tmp_path):
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, ["main.cpp"])

    # Debug builds abort on anomalies by design; disable the trap so this
    # test can observe the report-and-continue (Release) behavior everywhere.
    os.environ["CLICE_ANOMALY_NO_TRAP"] = "1"
    try:
        client = await make_client(executable, tmp_path)
    finally:
        os.environ.pop("CLICE_ANOMALY_NO_TRAP", None)

    try:
        await client.open_and_wait(tmp_path / "main.cpp")

        server_pid = client.server.pid
        workers = child_pids(server_pid)
        assert workers, "server should have spawned worker processes"
        # SIGABRT: dies via the same signal path as clice's own Debug traps,
        # exercises the crash handler, and is not intercepted by ASan
        # (handle_abort=0), unlike SIGSEGV which ASan turns into its own
        # report and a plain exit(1).
        os.kill(workers[0], signal.SIGABRT)

        for _ in range(50):
            if "WorkerCrash" in anomalies_in_log_messages(client):
                break
            await asyncio.sleep(0.2)
        assert "WorkerCrash" in anomalies_in_log_messages(client), (
            f"expected WorkerCrash anomaly, got messages: "
            f"{[m.message for m in client.log_messages]}"
        )
    finally:
        await shutdown_client(client)

    # The same marker must be greppable in the log files (this is what
    # assert_no_anomaly relies on for worker-side anomalies).
    assert any("WorkerCrash" in entry for entry in anomalies_in_log_files(tmp_path))

    # The abort also exercises the crash handler: the worker's backtrace
    # must land in its own log file — and ONLY there, never relayed into
    # the master log.
    logs_dir = tmp_path / ".clice" / "logs"
    worker_texts = [
        p.read_text(errors="replace")
        for p in logs_dir.rglob("*.log")
        if p.name != "master.log"
    ]
    assert any("CRASH STACK TRACE" in text for text in worker_texts), (
        "worker crash backtrace should be written to its log file"
    )
    master_texts = [p.read_text(errors="replace") for p in logs_dir.rglob("master.log")]
    assert master_texts and all(
        "CRASH STACK TRACE" not in text and "Stack dump" not in text
        for text in master_texts
    ), "worker backtrace must not leak into the master log"
