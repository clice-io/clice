"""Integration tests for the clice MasterServer using pygls."""

import asyncio
from pathlib import Path

import pytest
from lsprotocol.types import (
    ClientCapabilities,
    CodeActionContext,
    CodeActionParams,
    CompletionParams,
    DefinitionParams,
    DidCloseTextDocumentParams,
    DidOpenTextDocumentParams,
    DidChangeTextDocumentParams,
    DidSaveTextDocumentParams,
    DocumentLinkParams,
    DocumentSymbolParams,
    FoldingRangeParams,
    HoverParams,
    InitializeParams,
    InitializedParams,
    InlayHintParams,
    Position,
    Range,
    SemanticTokensParams,
    SignatureHelpParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    TextDocumentItem,
    VersionedTextDocumentIdentifier,
    WorkspaceFolder,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _workspace_uri(test_data_dir: Path, name: str = "hello_world") -> str:
    return (test_data_dir / name).as_uri()


def _file_uri(
    test_data_dir: Path, name: str = "hello_world", file: str = "main.cpp"
) -> str:
    return (test_data_dir / name / file).as_uri()


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


async def _initialize(client, test_data_dir: Path, name: str = "hello_world"):
    """Initialize with a workspace folder."""
    ws = test_data_dir / name
    result = await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=ws.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=ws.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())
    return result


async def _open_file(
    client, test_data_dir: Path, name: str = "hello_world", file: str = "main.cpp"
):
    """Open a text document."""
    path = test_data_dir / name / file
    content = path.read_text(encoding="utf-8")
    uri = path.as_uri()
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


async def _wait_for_compilation(client, uri: str, timeout: float = 30.0):
    """Wait for diagnostics on the given URI."""
    event = client.wait_for_diagnostics(uri)
    await asyncio.wait_for(event.wait(), timeout=timeout)


async def _open_and_wait(
    client,
    test_data_dir: Path,
    name: str = "hello_world",
    file: str = "main.cpp",
    timeout: float = 30.0,
):
    """Open file and wait for compilation diagnostics."""
    uri, content = await _open_file(client, test_data_dir, name, file)
    await _wait_for_compilation(client, uri, timeout)
    return uri, content


# ---------------------------------------------------------------------------
# Server info & capabilities
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_server_info(client, test_data_dir):
    result = await _initialize(client, test_data_dir)
    assert result.server_info.name == "clice"
    assert result.server_info.version == "0.1.0"


@pytest.mark.asyncio
async def test_capabilities(client, test_data_dir):
    result = await _initialize(client, test_data_dir)
    caps = result.capabilities
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
async def test_double_initialize_rejected(client, test_data_dir):
    await _initialize(client, test_data_dir)
    with pytest.raises(Exception):
        await client.initialize_async(
            InitializeParams(
                capabilities=ClientCapabilities(),
                workspace_folders=[],
            )
        )


@pytest.mark.asyncio
async def test_did_open_close_cycle(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_file(client, test_data_dir)
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_shutdown_exit(client, test_data_dir):
    await _initialize(client, test_data_dir)
    await client.shutdown_async(None)


@pytest.mark.asyncio
async def test_feature_requests_after_close(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_file(client, test_data_dir)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    assert result is None


# ---------------------------------------------------------------------------
# Document handling
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_incremental_change(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, content = await _open_file(client, test_data_dir)
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
async def test_diagnostics_received(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    assert uri in client.diagnostics
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# Feature requests (after compilation)
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_hover_before_compile(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_file(client, test_data_dir)
    result = await client.text_document_hover_async(
        HoverParams(text_document=_doc(uri), position=Position(line=0, character=0))
    )
    # May return null before compilation — that's fine
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_completion_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_completion_async(
        CompletionParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_signature_help_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_signature_help_async(
        SignatureHelpParams(
            text_document=_doc(uri), position=Position(line=0, character=0)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_definition_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri), position=Position(line=0, character=4)
        )
    )
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_document_symbol_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_document_symbol_async(
        DocumentSymbolParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_folding_range_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_folding_range_async(
        FoldingRangeParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_semantic_tokens_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_semantic_tokens_full_async(
        SemanticTokensParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_inlay_hint_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
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
async def test_code_action_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
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
async def test_document_link_request(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)
    result = await client.text_document_document_link_async(
        DocumentLinkParams(text_document=_doc(uri))
    )
    assert result is not None
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# Stress and edge cases
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_rapid_changes_stress(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, content = await _open_file(client, test_data_dir)
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
async def test_save_notification(client, test_data_dir):
    await _initialize(client, test_data_dir)
    uri, _ = await _open_file(client, test_data_dir)
    await asyncio.sleep(0.5)
    client.text_document_did_save(DidSaveTextDocumentParams(text_document=_doc(uri)))
    await asyncio.sleep(0.5)
    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.asyncio
async def test_hover_on_unknown_file(client, test_data_dir):
    await _initialize(client, test_data_dir)
    result = await client.text_document_hover_async(
        HoverParams(
            text_document=_doc("file:///nonexistent/fake.cpp"),
            position=Position(line=0, character=0),
        )
    )
    assert result is None


@pytest.mark.asyncio
async def test_all_features_after_compile_wait(client, test_data_dir):
    """After waiting for compilation, exercise all feature requests."""
    await _initialize(client, test_data_dir)
    uri, _ = await _open_and_wait(client, test_data_dir)

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
