"""Integration tests for the clice MasterServer using pygls."""

import asyncio
from pathlib import Path

import pytest
from conftest import CliceClient
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


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


# ---------------------------------------------------------------------------
# Server info & capabilities
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_server_info(client: CliceClient, workspace: Path):
    assert client.init_result.server_info.name == "clice"
    assert client.init_result.server_info.version == "0.1.0"


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_capabilities(client: CliceClient, workspace: Path):
    caps = client.init_result.capabilities
    assert caps.hover_provider is True
    assert caps.completion_provider is not None
    assert caps.definition_provider is True
    assert caps.document_symbol_provider is True
    assert caps.folding_range_provider is True
    assert caps.inlay_hint_provider is True
    assert caps.code_action_provider is True
    assert caps.semantic_tokens_provider is not None


# ---------------------------------------------------------------------------
# Initialization & shutdown
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_did_open_close_cycle(client: CliceClient, workspace: Path):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_shutdown_exit(client: CliceClient, workspace: Path):
    await client.shutdown_async(None)


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_feature_requests_after_close(client: CliceClient, workspace: Path):
    uri, _ = client.open(workspace / "main.cpp")
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    assert result is None


# ---------------------------------------------------------------------------
# Document handling
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_incremental_change(client: CliceClient, workspace: Path):
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


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_diagnostics_received(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert uri in client.diagnostics
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# Feature requests (after compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_hover_before_compile(client: CliceClient, workspace: Path):
    uri, _ = client.open(workspace / "main.cpp")
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    # May return null before compilation — that's fine
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_completion_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_signature_help_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_definition_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri), position=Position(line=0, character=4)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_document_symbol_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_document_symbol_async(
        DocumentSymbolParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_folding_range_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_folding_range_async(
        FoldingRangeParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_semantic_tokens_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_semantic_tokens_full_async(
        SemanticTokensParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_inlay_hint_request(client: CliceClient, workspace: Path):
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


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_code_action_request(client: CliceClient, workspace: Path):
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


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_document_link_request(client: CliceClient, workspace: Path):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.text_document_document_link_async(
        DocumentLinkParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# Stress and edge cases
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_rapid_changes_stress(client: CliceClient, workspace: Path):
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


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_save_notification(client: CliceClient, workspace: Path):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(0.5)
    client.text_document_did_save(DidSaveTextDocumentParams(text_document=_doc(uri)))
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_hover_on_unknown_file(client: CliceClient, workspace: Path):
    result = await client.text_document_hover_async(
        HoverParams(
            text_document=_doc("file:///nonexistent/fake.cpp"),
            position=Position(line=0, character=0),
        )
    )
    assert result is None


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_all_features_after_compile_wait(client: CliceClient, workspace: Path):
    """After waiting for compilation, exercise all feature requests."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    # Hover on 'add' (line 0, character 4)
    hover = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=4))
    )
    assert hover is not None

    # Completion
    completion = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri), position=Position(line=5, character=18)
        )
    )

    # Signature help
    await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )

    # Definition on 'add'
    await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri), position=Position(line=0, character=4)
        )
    )

    # Document symbols
    symbols = await client.text_document_document_symbol_async(
        DocumentSymbolParams(text_document=_doc(uri))
    )
    assert symbols is not None

    # Folding ranges
    folding = await client.text_document_folding_range_async(
        FoldingRangeParams(text_document=_doc(uri))
    )
    assert folding is not None

    # Semantic tokens
    tokens = await client.text_document_semantic_tokens_full_async(
        SemanticTokensParams(text_document=_doc(uri))
    )
    assert tokens is not None

    # Document links
    links = await client.text_document_document_link_async(
        DocumentLinkParams(text_document=_doc(uri))
    )
    assert links is not None

    # Code actions
    await client.text_document_code_action_async(
        CodeActionParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=0, character=10)
            ),
            context=CodeActionContext(diagnostics=[]),
        )
    )

    # Inlay hints
    await client.text_document_inlay_hint_async(
        InlayHintParams(
            text_document=_doc(uri),
            range=Range(
                start=Position(line=0, character=0), end=Position(line=10, character=0)
            ),
        )
    )

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
