"""Document-sync protocol edges: notifications that arrive outside the
expected lifecycle window, and replay of state that materialized before the
client handshake completed."""

import asyncio

import pytest
from lsprotocol.types import ClientCapabilities, InitializedParams, InitializeParams

from tests.conftest import check_no_anomaly, shutdown_client
from tests.integration.utils.assertions import get_errors, guidance_messages
from tests.integration.utils.client import CliceClient
from tests.integration.utils.workspace import did_change, write_cdb, write_source

TEST_TOML = (
    '[project]\ncache_dir = "${workspace}/.clice"\nenable_indexing = false\n'
    "\n[tracker]\ncdb_poll_seconds = 0\nworkspace_poll_seconds = 0\n"
)


@pytest.mark.workspace("hello_world")
async def test_open_before_initialize(request, executable, workspace):
    c = CliceClient()
    await c.start_io(str(executable), "serve")
    try:
        # didOpen racing ahead of the handshake is accepted; the session
        # must be fully usable once the server becomes ready.
        uri, _ = c.open(workspace / "main.cpp")
        await c.initialize(workspace)

        event = c.wait_for_diagnostics(uri)
        hover = await c.hover_at(uri, 2, 4)
        assert hover is not None and hover.contents is not None
        await asyncio.wait_for(event.wait(), timeout=60.0)
        assert get_errors(c.diagnostics[uri]) == []
    finally:
        await shutdown_client(c)
    check_no_anomaly(request, c)


@pytest.mark.workspace("hello_world")
async def test_change_without_open(client, workspace):
    uri = (workspace / "main.cpp").as_uri()
    # No didOpen baseline: the edit must be dropped.
    did_change(client, uri, 1, "int broken(")
    with pytest.raises(Exception, match="Document not open"):
        await client.hover_at(uri, 0, 0)
    # The dropped edit must not poison a later open.
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert get_errors(client.diagnostics[uri]) == []


@pytest.mark.workspace("hello_world")
async def test_version_regression_tolerated(client, workspace):
    uri, content = client.open(workspace / "main.cpp", version=5)
    # A version that goes backwards is a client bug; the edit is applied
    # anyway (and warned about server-side).
    event = client.wait_for_diagnostics(uri)
    did_change(client, uri, 3, content + "\nint bad(\n")
    await client.hover_at(uri, 0, 0)
    await asyncio.wait_for(event.wait(), timeout=60.0)
    assert get_errors(client.diagnostics[uri])


async def test_replay_after_late_handshake(request, executable, tmp_path):
    ws = tmp_path
    write_source(ws, "main.cpp", "int add(int a, int b) { return a + b; }\n")
    write_cdb(ws, ["main.cpp"])
    (ws / "clice.toml").write_text(TEST_TOML)

    c = CliceClient()
    await c.start_io(str(executable), "serve", f"--workspace={ws}")
    try:
        # The server is pre-initialized (ready); the client has not done its
        # handshake yet. Compile output materializes but must not be pushed.
        uri, _ = c.open(ws / "main.cpp")
        hover = await c.hover_at(uri, 0, 4)
        assert hover is not None
        assert uri not in c.diagnostics

        # A pre-initialized server rejects the initialize request; the
        # handshake still completes with the initialized notification, which
        # replays the materialized output.
        with pytest.raises(Exception):
            await c.initialize_async(
                InitializeParams(
                    capabilities=ClientCapabilities(), root_uri=ws.as_uri()
                )
            )
        event = c.wait_for_diagnostics(uri)
        c.initialized(InitializedParams())
        await asyncio.wait_for(event.wait(), timeout=30.0)
        assert get_errors(c.diagnostics[uri]) == []
    finally:
        c.workspace = ws
        await shutdown_client(c)
    check_no_anomaly(request, c)


async def test_startup_guidance_replayed(request, executable, tmp_path):
    ws = tmp_path
    write_source(ws, "main.cpp", "int x = 1;\n")
    (ws / "clice.toml").write_text(TEST_TOML)
    # No compile_commands.json: the headless workspace load emits guidance
    # before any client is attached; attaching must deliver the backlog.
    c = CliceClient()
    await c.start_io(str(executable), "serve", f"--workspace={ws}")
    try:
        for _ in range(100):
            if any("compile_commands.json" in m for m in guidance_messages(c)):
                break
            await asyncio.sleep(0.1)
        else:
            pytest.fail("startup guidance was not replayed to the late client")
    finally:
        c.workspace = ws
        await shutdown_client(c)
    check_no_anomaly(request, c)
