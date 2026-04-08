"""File operation tests for the clice LSP server."""

import asyncio

import pytest
from lsprotocol.types import (
    CompletionParams,
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
    SignatureHelpParams,
    VersionedTextDocumentIdentifier,
)

from tests.integration.utils import doc
from tests.integration.utils.workspace import did_change


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
        did_change(client, uri, i + 1, content)
    await asyncio.sleep(5)


@pytest.mark.workspace("clang_tidy")
async def test_clang_tidy(client, workspace):
    client.open(workspace / "main.cpp")
    await asyncio.sleep(5)


@pytest.mark.workspace("hello_world")
async def test_hover_save_close(client, workspace):
    main_cpp = workspace / "main.cpp"
    uri, content = client.open(main_cpp)
    hover = await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=2, character=4))
    )
    assert hover is not None
    assert hover.contents is not None
    await client.text_document_completion_async(
        CompletionParams(text_document=doc(uri), position=Position(line=0, character=0))
    )
    await client.text_document_signature_help_async(
        SignatureHelpParams(text_document=doc(uri), position=Position(line=0, character=0))
    )
    client.text_document_did_close(
        DidCloseTextDocumentParams(text_document=doc(uri))
    )
    closed_hover = await client.text_document_hover_async(
        HoverParams(text_document=doc(uri), position=Position(line=0, character=0))
    )
    assert closed_hover is None
