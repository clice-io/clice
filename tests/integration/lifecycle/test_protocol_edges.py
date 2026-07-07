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
        # must be fully usable once the server becomes ready. Register the
        # waiter before the handshake so a push emitted during it cannot
        # be missed.
        uri, _ = c.open(workspace / "main.cpp")
        event = c.wait_for_diagnostics(uri)
        await c.initialize(workspace)

        hover = await c.hover_at(uri, 2, 4)
        assert hover is not None and hover.contents is not None
        await asyncio.wait_for(event.wait(), timeout=60.0)
        assert get_errors(c.diagnostics[uri]) == []
    finally:
        await shutdown_client(c)
    check_no_anomaly(request, c)


@pytest.mark.workspace("hello_world")
async def test_close_before_initialize(request, executable, workspace):
    c = CliceClient()
    await c.start_io(str(executable), "serve")
    try:
        uri, _ = c.open(workspace / "main.cpp")
        c.close(uri)
        await c.initialize(workspace)
        # The pre-handshake close must not push a diagnostics clear (an
        # ungated one would be on the wire before the initialize response),
        # and the closed session must not be replayed.
        assert uri not in c.diagnostics
        with pytest.raises(Exception, match="Document not open"):
            await c.hover_at(uri, 0, 0)
        # The file closed before ready went through the reindex queue; a
        # normal open/compile cycle must still work afterwards.
        uri, _ = await c.open_and_wait(workspace / "main.cpp")
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
        # Non-vacuous: an ungated push is emitted during the compile the
        # hover awaits, so it would be on the wire before the hover
        # response and recorded by the time the hover future resolves.
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


async def test_no_stale_replay(request, executable, tmp_path):
    ws = tmp_path
    write_source(ws, "main.cpp", "int add(int a, int b) { return a + b; }\n")
    write_cdb(ws, ["main.cpp"])
    (ws / "clice.toml").write_text(TEST_TOML)

    c = CliceClient()
    await c.start_io(str(executable), "serve", f"--workspace={ws}")
    try:
        uri, content = c.open(ws / "main.cpp")
        hover = await c.hover_at(uri, 0, 4)
        assert hover is not None
        # An edit during the handshake window invalidates the materialized
        # output; the replay must skip it instead of pairing pre-edit
        # results with the new text.
        did_change(c, uri, 1, content + "int bad(\n")
        with pytest.raises(Exception):
            await c.initialize_async(
                InitializeParams(
                    capabilities=ClientCapabilities(), root_uri=ws.as_uri()
                )
            )
        c.initialized(InitializedParams())
        # A request round-trip orders us after the initialized processing:
        # a (wrong) replay push would already have been recorded.
        await c.query_context(uri)
        assert uri not in c.diagnostics
        # The next compile pushes fresh results for the edited buffer.
        event = c.wait_for_diagnostics(uri)
        await c.hover_at(uri, 0, 4)
        await asyncio.wait_for(event.wait(), timeout=30.0)
        assert get_errors(c.diagnostics[uri])
    finally:
        c.workspace = ws
        await shutdown_client(c)
    check_no_anomaly(request, c)


async def test_startup_guidance_delivered(request, executable, tmp_path):
    ws = tmp_path
    write_source(ws, "main.cpp", "int x = 1;\n")
    (ws / "clice.toml").write_text(TEST_TOML)
    # No compile_commands.json: the headless workspace load emits guidance
    # without waiting for any handshake; the client must still receive it
    # (drained from the server's notify log).
    c = CliceClient()
    await c.start_io(str(executable), "serve", f"--workspace={ws}")
    try:
        for _ in range(300):
            if any("compile_commands.json" in m for m in guidance_messages(c)):
                break
            await asyncio.sleep(0.1)
        else:
            pytest.fail("startup guidance never reached the client")
    finally:
        c.workspace = ws
        await shutdown_client(c)
    check_no_anomaly(request, c)
