"""Integration tests for import completion and buffer-aware module dependency features."""

import asyncio

import pytest
from lsprotocol.types import (
    CompletionParams,
    DidChangeTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


@pytest.mark.workspace("modules/chained_modules")
async def test_import_completion_basic(client, workspace):
    """Import completion should list known modules."""
    # First open mod_a to ensure it's scanned and module A is registered.
    await client.open_and_wait(workspace / "mod_a.cppm")

    # Open mod_b and change its content to an incomplete import line.
    uri_b, _ = client.open(workspace / "mod_b.cppm")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri_b, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text="import ")],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri_b),
            position=Position(line=0, character=7),
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "A" in labels, f"Expected 'A' in completion labels, got: {labels}"


@pytest.mark.workspace("modules/chained_modules")
async def test_import_completion_with_prefix(client, workspace):
    """Import completion with prefix should filter to matching modules."""
    # Open mod_a to register module A.
    await client.open_and_wait(workspace / "mod_a.cppm")

    # Open mod_b and type 'import A' (with prefix).
    uri_b, _ = client.open(workspace / "mod_b.cppm")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri_b, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text="import A")],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri_b),
            position=Position(line=0, character=8),
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "A" in labels, f"Expected 'A' in completion labels, got: {labels}"


@pytest.mark.workspace("modules/dotted_module_name")
async def test_import_completion_dotted_names(client, workspace):
    """Import completion should return dotted module names like my.app and my.io."""
    # Open both module files to register them.
    await client.open_and_wait(workspace / "io.cppm")
    await client.open_and_wait(workspace / "app.cppm")

    # Change app.cppm to an incomplete import with dotted prefix.
    uri_app, _ = client.open(workspace / "app.cppm")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri_app, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text="import my.")],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri_app),
            position=Position(line=0, character=10),
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "my.app" in labels or "my.io" in labels, (
        f"Expected dotted module names in completion labels, got: {labels}"
    )


@pytest.mark.workspace("modules/consumer_imports_module")
async def test_buffer_aware_module_deps(client, workspace):
    """Adding import in buffer (unsaved) should still build the needed PCM."""
    # Open the module file first so it gets scanned.
    await client.open_and_wait(workspace / "math.cppm")

    # Open main.cpp with new content that imports Math (simulating unsaved edit).
    uri, _ = client.open(workspace / "main.cpp")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(
                    text="import Math;\nint x = add(1, 2);\n"
                )
            ],
        )
    )

    # Trigger compilation via hover (pull-based model).
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(
            text_document=_doc(uri),
            position=Position(line=0, character=0),
        )
    )

    # Wait for diagnostics.
    await asyncio.wait_for(event.wait(), timeout=60.0)

    diags = client.diagnostics.get(uri, [])
    # Should have no errors if Math PCM was built successfully from buffer scan.
    errors = [d for d in diags if d.severity == 1]
    assert len(errors) == 0, f"Expected no errors, got: {errors}"
