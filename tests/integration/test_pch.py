"""Integration tests for PCH (precompiled header) functionality in MasterServer."""

import pytest
from lsprotocol.types import (
    CompletionParams,
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


@pytest.mark.workspace("pch_test")
async def test_pch_diagnostics_on_open(client, workspace):
    """Opening a file with #include should trigger PCH build and return clean diagnostics."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert uri in client.diagnostics
    # main.cpp is well-formed, so diagnostics list should be empty (no errors).
    diags = client.diagnostics[uri]
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("pch_test")
async def test_pch_body_edit_triggers_recompile(client, workspace):
    """Editing only the body (not the preamble) should trigger recompilation."""
    uri, content = await client.open_and_wait(workspace / "main.cpp")

    # Edit only the function body — preamble (#include "common.h") unchanged.
    new_content = content.replace("return result;", "return result + 1;")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text=new_content)],
        )
    )
    # Send hover to trigger recompilation via pull-based model.
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    assert uri in client.diagnostics
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("pch_test")
async def test_no_pch_for_no_includes(client, workspace):
    """A file with no #include directives should compile without PCH."""
    uri, _ = await client.open_and_wait(workspace / "no_includes.cpp")
    assert uri in client.diagnostics
    diags = client.diagnostics[uri]
    assert len(diags) == 0, f"Expected no diagnostics, got: {diags}"
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("pch_test")
async def test_hover_on_local_symbol(client, workspace):
    """Hover on a locally defined symbol should work when PCH is active."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # Hover over "add" on line 2 (0-indexed): "int add(int a, int b) {"
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=2, character=4))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("pch_test")
async def test_completion_with_pch(client, workspace):
    """Completion should see symbols from PCH headers."""
    uri, content = await client.open_and_wait(workspace / "main.cpp")

    # Add a line that starts typing "Poi" to trigger completion for Point.
    new_content = content + "\nPoi"
    lines = new_content.split("\n")
    last_line = len(lines) - 1

    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text=new_content)],
        )
    )

    # The completion request itself triggers compilation via ensure_compiled().
    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=last_line, character=3),
        )
    )
    # Completion should return results.
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
