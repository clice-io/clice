"""Integration tests for the clice MasterServer using pygls."""

import asyncio

import pytest
from lsprotocol.types import (
    CodeActionContext,
    CodeActionParams,
    CompletionParams,
    DefinitionParams,
    DidCloseTextDocumentParams,
    DidChangeTextDocumentParams,
    DidSaveTextDocumentParams,
    DocumentLinkParams,
    DocumentSymbolParams,
    FoldingRangeParams,
    HoverParams,
    InlayHintParams,
    Position,
    Range,
    SemanticTokensParams,
    SignatureHelpParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


@pytest.mark.workspace("hello_world")
async def test_server_info(client, workspace):
    assert client.init_result.server_info.name == "clice"
    assert client.init_result.server_info.version == "0.1.0"


@pytest.mark.workspace("hello_world")
async def test_capabilities(client, workspace):
    caps = client.init_result.capabilities
    assert caps.hover_provider is True
    assert caps.completion_provider is not None
    assert caps.definition_provider is True
    assert caps.document_symbol_provider is True
    assert caps.folding_range_provider is True
    assert caps.inlay_hint_provider is True
    assert caps.code_action_provider is True
    assert caps.semantic_tokens_provider is not None


@pytest.mark.workspace("hello_world")
async def test_did_open_close_cycle(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_shutdown_exit(client, workspace):
    await client.shutdown_async(None)


@pytest.mark.workspace("hello_world")
async def test_feature_requests_after_close(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    assert result is None


@pytest.mark.workspace("hello_world")
async def test_incremental_change(client, workspace):
    uri, content = client.open(workspace / "main.cpp")
    for i in range(5):
        content += f"\n// change {i}"
        client.text_document_did_change(
            DidChangeTextDocumentParams(
                text_document=VersionedTextDocumentIdentifier(uri=uri, version=i + 1),
                content_changes=[TextDocumentContentChangeWholeDocument(text=content)],
            )
        )
        await asyncio.sleep(0.05)
    await asyncio.sleep(1)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_diagnostics_received(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert uri in client.diagnostics
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_hover_before_compile(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_completion_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_signature_help_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_definition_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri), position=Position(line=2, character=4)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_document_symbol_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_document_symbol_async(
        DocumentSymbolParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_folding_range_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_folding_range_async(
        FoldingRangeParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_semantic_tokens_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_semantic_tokens_full_async(
        SemanticTokensParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_inlay_hint_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_inlay_hint_async(
        InlayHintParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=10, character=0)
            ),
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_code_action_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_code_action_async(
        CodeActionParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=0, character=10)
            ),
            context=CodeActionContext(diagnostics=[]),
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_document_link_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_document_link_async(
        DocumentLinkParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_rapid_changes_stress(client, workspace):
    uri, content = client.open(workspace / "main.cpp")
    for i in range(20):
        content += f"\n// stress change {i}\n"
        client.text_document_did_change(
            DidChangeTextDocumentParams(
                text_document=VersionedTextDocumentIdentifier(uri=uri, version=i + 1),
                content_changes=[TextDocumentContentChangeWholeDocument(text=content)],
            )
        )
    await asyncio.sleep(2)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_save_notification(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(0.5)
    client.text_document_did_save(DidSaveTextDocumentParams(text_document=_doc(uri)))
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("hello_world")
async def test_hover_on_unknown_file(client, workspace):
    result = await client.text_document_hover_async(
        HoverParams(
            text_document=_doc("file:///nonexistent/fake.cpp"),
            position=Position(line=0, character=0),
        )
    )
    assert result is None


@pytest.mark.workspace("hello_world")
async def test_all_features_after_compile_wait(client, workspace):
    """Exercise all feature requests after compilation completes."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    hover = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=2, character=4))
    )
    assert hover is not None

    completion = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri), position=Position(line=7, character=18)
        )
    )

    await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )

    await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri), position=Position(line=2, character=4)
        )
    )

    symbols = await client.text_document_document_symbol_async(
        DocumentSymbolParams(text_document=_doc(uri))
    )
    assert symbols is not None

    folding = await client.text_document_folding_range_async(
        FoldingRangeParams(text_document=_doc(uri))
    )
    assert folding is not None

    tokens = await client.text_document_semantic_tokens_full_async(
        SemanticTokensParams(text_document=_doc(uri))
    )
    assert tokens is not None

    links = await client.text_document_document_link_async(
        DocumentLinkParams(text_document=_doc(uri))
    )
    assert links is not None

    await client.text_document_code_action_async(
        CodeActionParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=0, character=10)
            ),
            context=CodeActionContext(diagnostics=[]),
        )
    )

    await client.text_document_inlay_hint_async(
        InlayHintParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=10, character=0)
            ),
        )
    )

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
