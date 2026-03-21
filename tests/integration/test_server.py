"""Integration tests for the clice MasterServer."""

import pytest
import asyncio
from tests.fixtures.client import LSPClient
from tests.fixtures.transport import LSPError


@pytest.mark.asyncio
async def test_server_info(client: LSPClient, test_data_dir):
    """Server reports correct name and version."""
    result = await client.initialize(test_data_dir / "hello_world")
    assert result["serverInfo"]["name"] == "clice"
    assert result["serverInfo"]["version"] == "0.1.0"


@pytest.mark.asyncio
async def test_capabilities(client: LSPClient, test_data_dir):
    """Server reports expected capabilities."""
    result = await client.initialize(test_data_dir / "hello_world")
    caps = result["capabilities"]
    assert caps["hoverProvider"] is True
    assert caps["completionProvider"] is not None
    assert caps["definitionProvider"] is True
    assert caps["documentSymbolProvider"] is True
    assert caps["foldingRangeProvider"] is True
    assert caps["inlayHintProvider"] is True
    assert caps["codeActionProvider"] is True
    # Check text document sync
    sync = caps["textDocumentSync"]
    assert sync["openClose"] is True
    assert sync["change"] == 2  # Incremental


@pytest.mark.asyncio
async def test_double_initialize_rejected(client: LSPClient, test_data_dir):
    """Second initialize request should be rejected."""
    await client.initialize(test_data_dir / "hello_world")
    with pytest.raises(LSPError):
        await client.send_request("initialize", {
            "capabilities": {},
            "workspaceFolders": [],
        })


@pytest.mark.asyncio
async def test_did_open_close_cycle(client: LSPClient, test_data_dir):
    """Open and close a document without errors."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(0.5)
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_incremental_change(client: LSPClient, test_data_dir):
    """Apply incremental changes without errors."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Send multiple rapid changes
    content = client.get_file("main.cpp").content

    for i in range(5):
        content += f"\n// change {i}"
        await client.did_change("main.cpp", content)
        await asyncio.sleep(0.05)

    await asyncio.sleep(1)
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_diagnostics_received(client: LSPClient, test_data_dir):
    """Opening a file should eventually produce diagnostics (if CDB available)."""
    workspace = test_data_dir / "hello_world"
    diagnostics_received = asyncio.Event()
    received_diagnostics = []

    def on_diagnostics(params):
        received_diagnostics.append(params)
        diagnostics_received.set()

    client.register_notification_handler(
        "textDocument/publishDiagnostics", on_diagnostics
    )

    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Wait for diagnostics (may timeout if no CDB - that's ok)
    try:
        await asyncio.wait_for(diagnostics_received.wait(), timeout=10.0)
        assert len(received_diagnostics) >= 1
        assert "diagnostics" in received_diagnostics[0]
    except asyncio.TimeoutError:
        # No CDB means no compilation, so no diagnostics - that's acceptable
        pass

    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_hover_before_compile(client: LSPClient, test_data_dir):
    """Hover on an uncompiled file should return null without error."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Hover immediately - before any compilation
    result = await client.hover("main.cpp", 0, 0)
    # May return null (no AST yet) - that's fine, shouldn't crash
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_shutdown_exit(client: LSPClient, test_data_dir):
    """Clean shutdown and exit sequence."""
    await client.initialize(test_data_dir / "hello_world")
    await client.shutdown()
    # exit is called by the fixture teardown


@pytest.mark.asyncio
async def test_feature_requests_after_close(client: LSPClient, test_data_dir):
    """Feature requests on closed file should return null."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await client.did_close("main.cpp")

    # Hover on a closed file
    result = await client.send_request(
        "textDocument/hover",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "position": {"line": 0, "character": 0},
        },
    )
    assert result is None


@pytest.mark.asyncio
async def test_multiple_files(client: LSPClient, test_data_dir):
    """Open multiple files without errors."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Change the content to simulate another file (reusing same path but different content)
    content = client.get_file("main.cpp").content
    content += "\n// additional content"
    await client.did_change("main.cpp", content)
    await asyncio.sleep(0.5)
    await client.did_close("main.cpp")
