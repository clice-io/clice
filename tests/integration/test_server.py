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
        await client.send_request(
            "initialize",
            {
                "capabilities": {},
                "workspaceFolders": [],
            },
        )


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
    """Opening a file should produce diagnostics."""
    workspace = test_data_dir / "hello_world"
    target_uri = (workspace / "main.cpp").as_uri()
    diagnostics_received = asyncio.Event()
    received_diagnostics = []

    def on_diagnostics(params):
        if params.get("uri") == target_uri:
            received_diagnostics.append(params)
            diagnostics_received.set()

    client.register_notification_handler(
        "textDocument/publishDiagnostics", on_diagnostics
    )

    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Wait for diagnostics — CDB is present so compilation should happen
    await asyncio.wait_for(diagnostics_received.wait(), timeout=15.0)
    assert len(received_diagnostics) >= 1
    assert "diagnostics" in received_diagnostics[0]

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


# ============================================================================
# Feature request tests (LSP method coverage)
# ============================================================================


async def _wait_for_compilation(client, workspace):
    """Helper: wait for diagnostics to confirm compilation finished."""
    target_uri = (workspace / "main.cpp").as_uri()
    diagnostics_received = asyncio.Event()

    def on_diagnostics(params):
        if params.get("uri") == target_uri:
            diagnostics_received.set()

    client.register_notification_handler(
        "textDocument/publishDiagnostics", on_diagnostics
    )

    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.wait_for(diagnostics_received.wait(), timeout=15.0)


@pytest.mark.asyncio
async def test_completion_request(client: LSPClient, test_data_dir):
    """Completion request should return a response after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.completion("main.cpp", 0, 0)
    # Completion at position (0,0) may return null or a list — just verify no error
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_signature_help_request(client: LSPClient, test_data_dir):
    """Signature help request should return a response after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.signature_help("main.cpp", 0, 0)
    # No active call expression at (0,0) so null is expected
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_definition_request(client: LSPClient, test_data_dir):
    """Go-to-definition request should return a response after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/definition",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "position": {"line": 2, "character": 4},  # 'main'
        },
    )
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_document_symbol_request(client: LSPClient, test_data_dir):
    """Document symbol request should return symbols after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/documentSymbol",
        {"textDocument": {"uri": (workspace / "main.cpp").as_uri()}},
    )
    assert result is not None, "Document symbols should not be null after compilation"
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_folding_range_request(client: LSPClient, test_data_dir):
    """Folding range request should return ranges after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/foldingRange",
        {"textDocument": {"uri": (workspace / "main.cpp").as_uri()}},
    )
    assert result is not None, "Folding ranges should not be null after compilation"
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_semantic_tokens_request(client: LSPClient, test_data_dir):
    """Semantic tokens request should return tokens after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/semanticTokens/full",
        {"textDocument": {"uri": (workspace / "main.cpp").as_uri()}},
    )
    assert result is not None, "Semantic tokens should not be null after compilation"
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_inlay_hint_request(client: LSPClient, test_data_dir):
    """Inlay hint request should return a response after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/inlayHint",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 10, "character": 0},
            },
        },
    )
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_code_action_request(client: LSPClient, test_data_dir):
    """Code action request should return a response after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/codeAction",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 0, "character": 10},
            },
            "context": {"diagnostics": []},
        },
    )
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_document_link_request(client: LSPClient, test_data_dir):
    """Document link request should return links after compilation."""
    workspace = test_data_dir / "hello_world"
    await _wait_for_compilation(client, workspace)

    result = await client.send_request(
        "textDocument/documentLink",
        {"textDocument": {"uri": (workspace / "main.cpp").as_uri()}},
    )
    assert result is not None, "Document links should not be null after compilation"
    assert len(result) >= 1, "Should find at least one link for #include <iostream>"
    await client.did_close("main.cpp")


# ============================================================================
# Stress and edge-case tests
# ============================================================================


@pytest.mark.asyncio
async def test_rapid_changes_stress(client: LSPClient, test_data_dir):
    """Rapid document changes should be handled without crashing."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")

    base_content = client.get_file("main.cpp").content

    # Send 20 rapid changes to stress the debounce mechanism
    for i in range(20):
        content = base_content + f"\n// stress change {i}\n"
        await client.did_change("main.cpp", content)
        # No sleep - fire as fast as possible

    # Give debounce timer time to settle
    await asyncio.sleep(2)
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_open_change_hover_close_cycle(client: LSPClient, test_data_dir):
    """Full lifecycle: open → change → hover → close repeated."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)

    for i in range(3):
        await client.did_open("main.cpp")
        content = client.get_file("main.cpp").content + f"\n// cycle {i}"
        await client.did_change("main.cpp", content)
        await asyncio.sleep(0.2)

        result = await client.hover("main.cpp", 0, 0)
        # Should not crash regardless of compilation state

        await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_save_notification(client: LSPClient, test_data_dir):
    """didSave notification should be handled without error."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(0.5)

    await client.did_save("main.cpp")
    await asyncio.sleep(0.5)
    await client.did_close("main.cpp")


@pytest.mark.asyncio
async def test_hover_on_unknown_file(client: LSPClient, test_data_dir):
    """Hover on a file that was never opened should return null."""
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)

    result = await client.send_request(
        "textDocument/hover",
        {
            "textDocument": {"uri": "file:///nonexistent/fake.cpp"},
            "position": {"line": 0, "character": 0},
        },
    )
    assert result is None


@pytest.mark.asyncio
async def test_all_features_after_compile_wait(client: LSPClient, test_data_dir):
    """After waiting for compilation, exercise all feature requests and verify responses."""
    workspace = test_data_dir / "hello_world"
    target_uri = (workspace / "main.cpp").as_uri()
    diagnostics_received = asyncio.Event()

    def on_diagnostics(params):
        if params.get("uri") == target_uri:
            diagnostics_received.set()

    client.register_notification_handler(
        "textDocument/publishDiagnostics", on_diagnostics
    )

    await client.initialize(workspace)
    await client.did_open("main.cpp")

    # Wait for compilation to finish (indicated by diagnostics)
    await asyncio.wait_for(diagnostics_received.wait(), timeout=15.0)

    uri = (workspace / "main.cpp").as_uri()

    # Hover on 'main' (line 2, character 4) — should return hover info
    hover = await client.hover("main.cpp", 2, 4)
    assert hover is not None, "Hover on 'main' should return non-null after compilation"

    # Completion at line 3, character 13 (after 'std::') — should return items
    completion = await client.completion("main.cpp", 3, 13)
    # completion may be None or a list/object — just verify we got a response without error

    # Signature help — may return null at arbitrary position
    sig_help = await client.signature_help("main.cpp", 0, 0)

    # Definition on 'main' — should return a location
    definition = await client.send_request(
        "textDocument/definition",
        {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 4}},
    )

    # Document symbols — should find at least 'main'
    symbols = await client.send_request(
        "textDocument/documentSymbol", {"textDocument": {"uri": uri}}
    )
    assert symbols is not None, (
        "Document symbols should return non-null after compilation"
    )

    # Folding ranges — should find at least the function body
    folding = await client.send_request(
        "textDocument/foldingRange", {"textDocument": {"uri": uri}}
    )
    assert folding is not None, (
        "Folding ranges should return non-null after compilation"
    )

    # Semantic tokens — should return token data
    tokens = await client.send_request(
        "textDocument/semanticTokens/full", {"textDocument": {"uri": uri}}
    )
    assert tokens is not None, (
        "Semantic tokens should return non-null after compilation"
    )

    # Document links — #include <iostream> should produce a link
    links = await client.send_request(
        "textDocument/documentLink", {"textDocument": {"uri": uri}}
    )
    assert links is not None, "Document links should return non-null after compilation"
    assert len(links) >= 1, "Should find at least one document link for #include"

    # Code action — may return empty list
    actions = await client.send_request(
        "textDocument/codeAction",
        {
            "textDocument": {"uri": uri},
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 0, "character": 10},
            },
            "context": {"diagnostics": []},
        },
    )

    # Inlay hints
    hints = await client.send_request(
        "textDocument/inlayHint",
        {
            "textDocument": {"uri": uri},
            "range": {
                "start": {"line": 0, "character": 0},
                "end": {"line": 10, "character": 0},
            },
        },
    )

    await client.did_close("main.cpp")
