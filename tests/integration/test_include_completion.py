"""Integration tests for #include completion in clice."""

import pytest
from lsprotocol.types import (
    CompletionParams,
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    Position,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


@pytest.mark.workspace("include_completion")
async def test_include_completion_quoted(client, workspace):
    """Completion after #include " should list local headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # Update content to trigger include completion for "my" prefix.
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(text='#include "my')
            ],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=0, character=12),  # After "my"
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "myheader.h" in labels

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("include_completion")
async def test_include_completion_subdirectory(client, workspace):
    """Completion for #include "subdir/ should list files in subdir."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(text='#include "subdir/')
            ],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=0, character=17),  # After "subdir/"
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "nested.h" in labels

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("include_completion")
async def test_include_completion_angle_bracket(client, workspace):
    """Completion after #include < should list system headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[
                TextDocumentContentChangeWholeDocument(text="#include <cstd")
            ],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=0, character=14),  # After "cstd"
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    # Should contain at least some standard library headers starting with "cstd".
    cstd_labels = [name for name in labels if name.startswith("cstd")]
    assert len(cstd_labels) > 0, f"Expected cstd* headers, got: {labels}"

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("include_completion")
async def test_no_include_completion_on_regular_code(client, workspace):
    """Regular code should NOT trigger include completion (goes to worker)."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text="int x = ")],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=0, character=8),
        )
    )

    # Should return results from clang (keywords, etc.), not include paths.
    # Verify none of the results look like header filenames.
    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    assert "myheader.h" not in labels
    assert "nested.h" not in labels

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("include_completion")
async def test_include_completion_empty_prefix(client, workspace):
    """Completion after #include " with no prefix should list all local headers."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text='#include "')],
        )
    )

    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri),
            position=Position(line=0, character=10),  # Right after the quote
        )
    )

    assert result is not None
    items = result.items if hasattr(result, "items") else result
    labels = [item.label for item in items]
    # With empty prefix, should list available headers including myheader.h
    # and the subdir/ directory entry.
    assert "myheader.h" in labels

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
