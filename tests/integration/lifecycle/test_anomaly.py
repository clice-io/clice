"""End-to-end check of the anomaly reporting machinery.

Kills a worker process and verifies the master reports `[anomaly:worker_crash]`
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

        server_pid = client._server.pid
        workers = child_pids(server_pid)
        assert workers, "server should have spawned worker processes"
        os.kill(workers[0], signal.SIGKILL)

        for _ in range(50):
            if "worker_crash" in anomalies_in_log_messages(client):
                break
            await asyncio.sleep(0.2)
        assert "worker_crash" in anomalies_in_log_messages(client), (
            f"expected worker_crash anomaly, got messages: "
            f"{[m.message for m in client.log_messages]}"
        )
    finally:
        await shutdown_client(client)

    # The same marker must be greppable in the log files (this is what
    # assert_no_anomaly relies on for worker-side anomalies).
    assert any("worker_crash" in entry for entry in anomalies_in_log_files(tmp_path))
