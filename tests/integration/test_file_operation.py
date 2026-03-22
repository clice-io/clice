import pytest
import asyncio
from tests.fixtures.client import LSPClient


@pytest.mark.asyncio
async def test_did_open(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "hello_world")
    await client.did_open("main.cpp")
    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_did_change(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "hello_world")
    await client.did_open("main.cpp")

    # Test frequently change content will not make server crash.
    content = client.get_file("main.cpp").content

    for _ in range(0, 20):
        content += "\n"
        await asyncio.sleep(0.2)
        await client.did_change("main.cpp", content)

    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_clang_tidy(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "clang_tidy")
    await client.did_open("main.cpp")
    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_hover_save_close(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "hello_world"
    diagnostics_received = asyncio.Event()

    def on_diagnostics(params):
        diagnostics_received.set()

    client.register_notification_handler(
        "textDocument/publishDiagnostics", on_diagnostics
    )

    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Wait for initial compilation to finish
    await asyncio.wait_for(diagnostics_received.wait(), timeout=15.0)

    content = client.get_file("main.cpp").content + "\nint saved = 1;\n"
    await client.did_change("main.cpp", content)
    await client.did_save("main.cpp", include_text=True)

    # Wait for recompilation after change
    diagnostics_received.clear()
    await asyncio.wait_for(diagnostics_received.wait(), timeout=15.0)

    hover = await client.hover("main.cpp", 2, 4)  # hover on 'main'
    assert hover is not None
    assert "contents" in hover

    # Completion and signature help at (0,0) may return empty results
    # — the important thing is they don't crash
    completion = await client.completion("main.cpp", 0, 0)
    signature_help = await client.signature_help("main.cpp", 0, 0)

    # Cancellation for unknown requests should not affect normal requests.
    await client.send_notification("$/cancelRequest", {"id": 99999})
    await client.did_close("main.cpp")

    closed_hover = await client.send_request(
        "textDocument/hover",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "position": {"line": 0, "character": 0},
        },
    )
    assert closed_hover is None
