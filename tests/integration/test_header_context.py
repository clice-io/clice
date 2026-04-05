"""Integration tests for header context: IDE features in header files."""

import asyncio

import pytest
from lsprotocol.types import (
    CompletionParams,
    HoverParams,
    Position,
    TextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


@pytest.mark.workspace("header_context")
async def test_hover_in_header_file(client, workspace):
    """Opening a header file and hovering should work via header context."""
    # First open main.cpp so the server scans dependencies
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # Now open the header file
    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    # Hover on 'add' function (line 2, character 11 = 'add')
    hover = await asyncio.wait_for(
        client.text_document_hover_async(
            HoverParams(
                text_document=_doc(utils_uri),
                position=Position(line=2, character=11),
            )
        ),
        timeout=30.0,
    )
    assert hover is not None, (
        "Hover in header file should return result via header context"
    )
    assert hover.contents is not None


@pytest.mark.workspace("header_context")
async def test_diagnostics_in_header_file(client, workspace):
    """Header files should get diagnostics via header context."""
    main_uri, _ = await client.open_and_wait(workspace / "main.cpp")

    utils_h = workspace / "utils.h"
    utils_uri, _ = client.open(utils_h)

    # Register diagnostics listener BEFORE triggering compilation
    event = client.wait_for_diagnostics(utils_uri)

    # Trigger compilation via hover
    await asyncio.wait_for(
        client.text_document_hover_async(
            HoverParams(
                text_document=_doc(utils_uri),
                position=Position(line=0, character=0),
            )
        ),
        timeout=30.0,
    )

    # Wait for diagnostics — should have 0 errors for a valid header
    await asyncio.wait_for(event.wait(), timeout=30.0)
    diags = client.diagnostics.get(utils_uri, [])
    assert len(diags) == 0, f"Expected no diagnostics for valid header, got: {diags}"
