"""A client that never drains stderr must not be able to wedge the server."""

import asyncio

import pytest

from tests.tools.compile_commands import write_cdb
from tests.tools.lifecycle import make_client, shutdown_client


@pytest.mark.timeout(600)
async def test_stderr_flood_never_wedges(executable, tmp_path):
    # Editors drain stderr; a client that refuses to must cost log lines,
    # never liveness. Each hover emits info-level perf lines AND doubles as
    # the liveness probe: before the fix the event loop parked in write(2)
    # once the pipe filled (~1000 hovers in) and never answered again.
    (tmp_path / "probe.cpp").write_text("int value = 42;\n")
    write_cdb(tmp_path, ["probe.cpp"])

    client = await make_client(executable, tmp_path, drain_stderr=False)
    try:
        uri, _ = client.open(tmp_path / "probe.cpp")
        for i in range(2000):
            try:
                hover = await asyncio.wait_for(
                    client.hover_at(uri, 0, 5, timeout=15), timeout=18
                )
            except TimeoutError:
                # Kill the wedged server first: a graceful teardown against
                # a process that no longer reads stdin has nothing to wait
                # for.
                client.kill_server()
                pytest.fail(f"server wedged after {i} hovers (stderr backpressure)")
            assert hover is not None, f"empty hover at {i}"
    finally:
        # Hostile phase over: resume draining so teardown can observe pipe
        # EOF (asyncio's Process.wait() waits on it) and collect the gap
        # report the sink emits once writes flow again.
        client.spawn_stderr_pump()
        await shutdown_client(client)
    assert b"client not draining" in client.drained_stderr()
