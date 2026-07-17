"""A client that never drains stderr must not be able to wedge the server."""

import asyncio

import pytest

from tests.tools.compile_commands import write_cdb
from tests.tools.lifecycle import make_client, shutdown_client

FLOOD_LINES = 3000
FLOOD_SIZE = 256


async def test_log_flood_gated(executable, tmp_path):
    # The load-generating hook must not exist for ordinary clients.
    (tmp_path / "probe.cpp").write_text("int value = 42;\n")
    write_cdb(tmp_path, ["probe.cpp"])
    client = await make_client(executable, tmp_path)
    try:
        with pytest.raises(Exception):
            await asyncio.wait_for(
                client.protocol.send_request_async(
                    "clice/internal/logFlood", {"count": 1, "size": 16}
                ),
                timeout=10,
            )
    finally:
        await shutdown_client(client)


def flood_lines_in(text: str) -> int:
    return text.count("[stderr-flood ")


@pytest.mark.timeout(600)
async def test_stderr_flood_never_wedges(executable, tmp_path):
    # Editors drain stderr; a client that refuses to must cost mirror
    # lines — never liveness, and never file-log completeness. The volume
    # comes from a test hook so it is deterministic: feature log lines
    # change shape over time and must not be load-bearing here. Before the
    # fix the event loop parked in write(2) once the pipe filled (~1000
    # info lines in) and never answered again.
    (tmp_path / "probe.cpp").write_text("int value = 42;\n")
    write_cdb(tmp_path, ["probe.cpp"])

    client = await make_client(
        executable,
        tmp_path,
        drain_stderr=False,
        initialization_options={"project": {"test_hooks": True}},
    )
    try:
        uri, _ = client.open(tmp_path / "probe.cpp")
        # ~1MB in ten batches, far past the pipe (~196KB with asyncio's
        # reader buffer) plus the sink's 256KB buffer budget; each hover
        # in between is a bounded liveness probe.
        for batch in range(10):
            try:
                await asyncio.wait_for(
                    client.protocol.send_request_async(
                        "clice/internal/logFlood",
                        {"count": FLOOD_LINES // 10, "size": FLOOD_SIZE},
                    ),
                    timeout=18,
                )
                hover = await asyncio.wait_for(
                    client.hover_at(uri, 0, 5, timeout=15), timeout=18
                )
            except TimeoutError:
                # Kill the wedged server first: a graceful teardown against
                # a process that no longer reads stdin has nothing to wait
                # for.
                client.kill_server()
                pytest.fail(f"server wedged in batch {batch} (stderr backpressure)")
            assert hover is not None, f"empty hover in batch {batch}"
    finally:
        # Hostile phase over: resume draining so teardown can observe pipe
        # EOF (asyncio's Process.wait() waits on it) and collect the gap
        # report the sink emits once writes flow again.
        client.spawn_stderr_pump()
        await shutdown_client(client)

    # Shedding happened where intended: the mirror lost flood lines and
    # reported the gap...
    drained = client.drained_stderr().decode("utf-8", errors="replace")
    assert "client not draining" in drained
    assert flood_lines_in(drained) < FLOOD_LINES

    # ...while the file log kept every single one: the mirror is
    # best-effort, the file log is the record.
    logs_dir = tmp_path / ".clice" / "logs"
    file_text = "".join(
        p.read_text(errors="replace") for p in logs_dir.rglob("master.log")
    )
    assert flood_lines_in(file_text) == FLOOD_LINES
