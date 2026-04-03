"""File operation tests for the clice LSP server."""

import asyncio

import pytest
from lsprotocol.types import (
    CompletionParams,
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
    SignatureHelpParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


@pytest.mark.workspace("hello_world")
async def test_did_open(client, workspace):
    client.open(workspace / "main.cpp")
    await asyncio.sleep(5)


@pytest.mark.workspace("hello_world")
async def test_did_change(client, workspace):
    uri, content = client.open(workspace / "main.cpp")

    for i in range(20):
        content += "\n"
        await asyncio.sleep(0.2)
        client.text_document_did_change(
            DidChangeTextDocumentParams(
                text_document=VersionedTextDocumentIdentifier(uri=uri, version=i + 1),
                content_changes=[TextDocumentContentChangeWholeDocument(text=content)],
            )
        )
    await asyncio.sleep(5)


@pytest.mark.workspace("clang_tidy")
async def test_clang_tidy(client, workspace):
    client.open(workspace / "main.cpp")
    await asyncio.sleep(5)


@pytest.mark.workspace("hello_world")
async def test_hover_save_close(client, workspace):
    main_cpp = workspace / "main.cpp"

    uri, content = client.open(main_cpp)

    # Hover on 'add' — this triggers ensure_compiled() which compiles the file
    hover = await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=4),
        )
    )
    assert hover is not None
    assert hover.contents is not None

    # Completion and signature help at (0,0) — just verify no crash
    await client.text_document_completion_async(
        CompletionParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )
    await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )

    # Close
    client.text_document_did_close(
        DidCloseTextDocumentParams(text_document=TextDocumentIdentifier(uri=uri))
    )

    # Hover on closed file should return null
    closed_hover = await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )
    assert closed_hover is None
