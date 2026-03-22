"""File operation tests for the clice LSP server using pygls."""

import asyncio

import pytest
from lsprotocol.types import (
    ClientCapabilities,
    CompletionParams,
    DidChangeTextDocumentParams,
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    DidSaveTextDocumentParams,
    HoverParams,
    InitializeParams,
    InitializedParams,
    Position,
    SignatureHelpParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    TextDocumentItem,
    VersionedTextDocumentIdentifier,
    WorkspaceFolder,
)


async def _init(client, workspace):
    await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=workspace.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=workspace.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())


def _open(client, path):
    uri = path.as_uri()
    content = path.read_text(encoding="utf-8")
    client.text_document_did_open(
        DidOpenTextDocumentParams(
            text_document=TextDocumentItem(
                uri=uri,
                language_id="cpp",
                version=0,
                text=content,
            )
        )
    )
    return uri, content


@pytest.mark.asyncio
async def test_did_open(client, test_data_dir):
    workspace = test_data_dir / "hello_world"
    await _init(client, workspace)
    _open(client, workspace / "main.cpp")
    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_did_change(client, test_data_dir):
    workspace = test_data_dir / "hello_world"
    await _init(client, workspace)
    uri, content = _open(client, workspace / "main.cpp")

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


@pytest.mark.asyncio
async def test_clang_tidy(client, test_data_dir):
    workspace = test_data_dir / "clang_tidy"
    await _init(client, workspace)
    _open(client, workspace / "main.cpp")
    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_hover_save_close(client, test_data_dir):
    workspace = test_data_dir / "hello_world"
    main_cpp = workspace / "main.cpp"
    await _init(client, workspace)

    uri, content = _open(client, main_cpp)

    # Wait for initial compilation
    event = client.wait_for_diagnostics(uri)
    await asyncio.wait_for(event.wait(), timeout=30.0)

    # Change and save
    content += "\nint saved = 1;\n"
    event = client.wait_for_diagnostics(uri)
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=1),
            content_changes=[TextDocumentContentChangeWholeDocument(text=content)],
        )
    )
    client.text_document_did_save(
        DidSaveTextDocumentParams(text_document=TextDocumentIdentifier(uri=uri))
    )

    # Wait for recompilation
    await asyncio.wait_for(event.wait(), timeout=30.0)

    # Hover on 'add'
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
