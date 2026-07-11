"""Crash-recovery of background indexing.

Kills stateless workers while an indexing round is in flight and verifies the
round still converges: in-flight files fail with worker_crashed, the indexer
requeues them, and a follow-up round indexes every file.
"""

import asyncio
import os
import signal
import sys
from pathlib import Path

import pytest
from lsprotocol.types import WorkspaceSymbolParams

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb

FILE_COUNT = 20


def stateless_worker_pids(server_pid: int) -> list[int]:
    pids = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            cmdline = (entry / "cmdline").read_bytes()
        except OSError:
            continue
        ppid = int(stat.rsplit(")", 1)[1].split()[1])
        if ppid == server_pid and b"SL-" in cmdline:
            pids.append(int(entry.name))
    return pids


async def indexed_functions(client) -> set[str]:
    result = await client.workspace_symbol_async(WorkspaceSymbolParams(query="func_"))
    return {s.name for s in result or []}


@pytest.mark.skipif(sys.platform != "linux", reason="worker discovery uses /proc")
@pytest.mark.allow_anomaly
async def test_crash_during_indexing(executable, tmp_path):
    # Enough moderately heavy TUs that the indexing round is still in flight
    # when the workers get killed.
    files = []
    for i in range(FILE_COUNT):
        name = f"file_{i}.cpp"
        (tmp_path / name).write_text(
            f"#include <vector>\n#include <string>\n"
            f'int func_{i}() {{ return (int)std::string("{i}").size(); }}\n'
        )
        files.append(name)
    (tmp_path / "main.cpp").write_text("int main() { return 0; }\n")
    write_cdb(tmp_path, files + ["main.cpp"])

    # The kills below surface as WorkerCrash anomalies; Debug builds abort on
    # anomalies by design, so disable the trap like test_anomaly does.
    os.environ["CLICE_ANOMALY_NO_TRAP"] = "1"
    try:
        client = await make_client(executable, tmp_path)
    finally:
        os.environ.pop("CLICE_ANOMALY_NO_TRAP", None)

    try:
        uri, _ = await client.open_and_wait(tmp_path / "main.cpp")

        # Wait until the round demonstrably started (first symbols merged),
        # then kill a stateless worker mid-round.
        killed = False
        for _ in range(300):
            if await indexed_functions(client):
                workers = stateless_worker_pids(client.server.pid)
                if workers:
                    os.kill(workers[0], signal.SIGKILL)
                    killed = True
                break
            await asyncio.sleep(0.1)
        assert killed, "indexing never started or no stateless worker found"

        # The files that were in flight on the killed worker must be
        # requeued and indexed by a follow-up round: every function
        # eventually appears in the project index.
        expected = {f"func_{i}" for i in range(FILE_COUNT)}
        found: set[str] = set()
        for _ in range(120):
            found = await indexed_functions(client)
            if expected <= found:
                break
            await asyncio.sleep(1)
        assert expected <= found, f"missing after crash: {sorted(expected - found)}"
    finally:
        await shutdown_client(client)
