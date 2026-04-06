"""Integration tests for header context LSP extension commands.

Tests the clice/queryContext, clice/currentContext, and clice/switchContext
extension commands that allow switching the compilation context for header files.

utils.h uses std::vector without including <vector> itself -- it depends on
main.cpp to provide that include.  Without header context resolution, the
server cannot compile utils.h at all.
"""

import asyncio

import pytest
from lsprotocol.types import (
    HoverParams,
    Position,
    TextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


def _get(obj, key, default=None):
    """Access a field from either a dict or an object with attributes."""
    if isinstance(obj, dict):
        return obj.get(key, default)
    return getattr(obj, key, default)


@pytest.mark.workspace("header_context")
async def test_query_context_returns_host_sources(client, workspace):
    """clice/queryContext on a header should return source files that include it."""
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    result = await asyncio.wait_for(
        client.protocol.send_request_async("clice/queryContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    assert result is not None
    total = _get(result, "total")
    contexts = _get(result, "contexts", [])
    assert total >= 1, f"Should find at least main.cpp as context, got total={total}"
    # Check that main.cpp is among the contexts.
    uris = [_get(c, "uri") for c in contexts]
    assert any("main.cpp" in u for u in uris), (
        f"main.cpp should be listed as a context option, got: {uris}"
    )


@pytest.mark.workspace("header_context")
async def test_query_context_source_file_empty(client, workspace):
    """clice/queryContext on a source file should return empty contexts."""
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    result = await asyncio.wait_for(
        client.protocol.send_request_async("clice/queryContext", {"uri": main_uri}),
        timeout=30.0,
    )
    assert result is not None
    assert _get(result, "total") == 0
    assert len(_get(result, "contexts", [])) == 0


@pytest.mark.workspace("header_context")
async def test_current_context_default_null(client, workspace):
    """clice/currentContext should return null context by default."""
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    result = await asyncio.wait_for(
        client.protocol.send_request_async("clice/currentContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    assert result is not None
    assert _get(result, "context") is None, (
        "Default context should be null (no explicit override)"
    )


@pytest.mark.workspace("header_context")
async def test_switch_context_and_current_context(client, workspace):
    """switchContext should set the active context, currentContext should reflect it."""
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    # Switch context to main.cpp.
    switch_result = await asyncio.wait_for(
        client.protocol.send_request_async(
            "clice/switchContext",
            {"uri": utils_uri, "contextUri": main_uri},
        ),
        timeout=30.0,
    )
    assert switch_result is not None
    assert _get(switch_result, "success") is True

    # Verify currentContext now returns main.cpp.
    current = await asyncio.wait_for(
        client.protocol.send_request_async("clice/currentContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    assert current is not None
    ctx = _get(current, "context")
    assert ctx is not None, (
        "After switchContext, currentContext should return the active context"
    )
    assert "main.cpp" in _get(ctx, "uri")


@pytest.mark.workspace("header_context")
async def test_full_context_flow(client, workspace):
    """Full flow: open, query, switch, verify hover works in header context."""
    # 1. Open main.cpp, wait for initial compile.
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # 2. Open utils.h (non self-contained header using std::vector).
    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    # 3. queryContext on utils.h -> should return main.cpp as a context option.
    query = await asyncio.wait_for(
        client.protocol.send_request_async("clice/queryContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    assert _get(query, "total") >= 1
    contexts = _get(query, "contexts", [])
    context_uris = [_get(c, "uri") for c in contexts]
    assert any("main.cpp" in u for u in context_uris)

    # 4. currentContext on utils.h -> should be null (default).
    current = await asyncio.wait_for(
        client.protocol.send_request_async("clice/currentContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    assert _get(current, "context") is None

    # 5. switchContext on utils.h to main.cpp.
    switch = await asyncio.wait_for(
        client.protocol.send_request_async(
            "clice/switchContext",
            {"uri": utils_uri, "contextUri": main_uri},
        ),
        timeout=30.0,
    )
    assert _get(switch, "success") is True

    # 6. currentContext on utils.h -> should now be main.cpp.
    current2 = await asyncio.wait_for(
        client.protocol.send_request_async("clice/currentContext", {"uri": utils_uri}),
        timeout=30.0,
    )
    ctx = _get(current2, "context")
    assert ctx is not None
    assert "main.cpp" in _get(ctx, "uri")

    # 7. Hover on 'sum' function in utils.h -> should work (proves header compiled).
    diag_event = client.wait_for_diagnostics(utils_uri)
    hover = await asyncio.wait_for(
        client.text_document_hover_async(
            HoverParams(
                text_document=_doc(utils_uri),
                position=Position(line=12, character=12),  # 'sum' function
            )
        ),
        timeout=30.0,
    )
    assert hover is not None, "Hover on 'sum' in header should work after switchContext"

    # 8. Check diagnostics on utils.h -> should have 0 errors.
    await asyncio.wait_for(diag_event.wait(), timeout=30.0)
    diags = client.diagnostics.get(utils_uri, [])
    errors = [d for d in diags if d.severity == 1]
    assert len(errors) == 0, (
        f"Header should have no errors after switchContext, got: {errors}"
    )
