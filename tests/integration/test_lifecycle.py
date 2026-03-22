"""Lifecycle tests for the clice LSP server using pygls."""

import pytest
from lsprotocol.types import (
    ClientCapabilities,
    HoverParams,
    CompletionParams,
    SignatureHelpParams,
    InitializeParams,
    InitializedParams,
    DidOpenTextDocumentParams,
    Position,
    TextDocumentIdentifier,
    TextDocumentItem,
    WorkspaceFolder,
)


@pytest.mark.asyncio
async def test_initialize(client, test_data_dir):
    ws = test_data_dir / "hello_world"
    result = await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=ws.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=ws.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())
    assert result.server_info is not None
    assert result.server_info.name == "clice"


@pytest.mark.asyncio
async def test_shutdown_rejects_feature_requests(client, test_data_dir):
    ws = test_data_dir / "hello_world"
    await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=ws.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=ws.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())

    uri = (ws / "main.cpp").as_uri()
    content = (ws / "main.cpp").read_text(encoding="utf-8")
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

    await client.shutdown_async(None)

    doc = TextDocumentIdentifier(uri=uri)
    pos = Position(line=0, character=0)

    with pytest.raises(Exception):
        await client.text_document_hover_async(
            HoverParams(text_document=doc, position=pos)
        )

    with pytest.raises(Exception):
        await client.text_document_completion_async(
            CompletionParams(text_document=doc, position=pos)
        )

    with pytest.raises(Exception):
        await client.text_document_signature_help_async(
            SignatureHelpParams(text_document=doc, position=pos)
        )
