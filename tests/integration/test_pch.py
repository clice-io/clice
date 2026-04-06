"""Integration tests for PCH (precompiled header) functionality in MasterServer."""

import asyncio

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
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    await asyncio.wait_for(event.wait(), timeout=30.0)
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


@pytest.mark.workspace("pch_test")
async def test_preamble_edit_then_hover(client, workspace):
    """After editing the preamble (adding an #include), AST should still work."""
    uri, content = await client.open_and_wait(workspace / "main.cpp")

    # Verify initial state is clean.
    diags = client.diagnostics.get(uri, [])
    assert len(diags) == 0, f"Expected no initial diagnostics, got: {diags}"

    # Edit the preamble: add a second #include (triggers PCH rebuild).
    # Use project-local header instead of system header (<cstdio>) to avoid
    # slow PCH rebuilds on macOS CI that cause SIGPIPE timeouts.
    new_content = '#include "common.h"\n#include "common.h"\n' + "\n".join(
        content.split("\n")[1:]
    )
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text=new_content)],
        )
    )

    # Trigger recompilation via hover — this will rebuild PCH with new preamble.
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=3, character=4))
    )
    await asyncio.wait_for(event.wait(), timeout=60.0)

    # AST should still be valid — no errors.
    diags = client.diagnostics.get(uri, [])
    errors = [d for d in diags if d.severity == 1]
    assert len(errors) == 0, f"Expected no errors after preamble edit, got: {errors}"

    # Hover should still work on a symbol.
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=3, character=4))
    )
    assert result is not None, "Hover failed after preamble edit"
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("pch_test")
async def test_preamble_edit_multiple_times(client, workspace):
    """Multiple preamble edits should not break AST building."""
    uri, content = await client.open_and_wait(workspace / "main.cpp")

    for i in range(3):
        # Add progressively more includes.
        includes = '#include "common.h"\n'
        for j in range(i + 1):
            includes += f"// edit {j}\n"
        new_content = includes + "\n".join(content.split("\n")[1:])

        version = i + 1
        client.text_document_did_change(
            DidChangeTextDocumentParams(
                text_document=VersionedTextDocumentIdentifier(uri=uri, version=version),
                content_changes=[
                    TextDocumentContentChangeWholeDocument(text=new_content)
                ],
            )
        )

        event = client.wait_for_diagnostics(uri)
        await client.text_document_hover_async(
            HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
        )
        await asyncio.wait_for(event.wait(), timeout=60.0)

    # After multiple edits, should still be clean.
    diags = client.diagnostics.get(uri, [])
    errors = [d for d in diags if d.severity == 1]
    assert len(errors) == 0, (
        f"Expected no errors after multiple preamble edits, got: {errors}"
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
