"""Integration tests for the clice MasterServer using pygls."""

import asyncio

import pytest
from lsprotocol.types import (
    DidSaveTextDocumentParams,
    Position,
    Range,
)

from tests.integration.utils import doc
from tests.integration.utils.wait import SETTLE_TIME
from tests.integration.utils.workspace import did_change


@pytest.mark.workspace("hello_world")
async def test_server_info(client, workspace):
    assert client.init_result.server_info.name == "clice"
    assert client.init_result.server_info.version == "0.1.0"


@pytest.mark.workspace("hello_world")
async def test_capabilities(client, workspace):
    def capability_enabled(capability: object) -> bool:
        return capability is True or (
            capability is not None and capability is not False
        )

    caps = client.init_result.capabilities
    assert caps.hover_provider is True
    assert caps.completion_provider is not None
    assert capability_enabled(caps.definition_provider)
    assert capability_enabled(caps.document_symbol_provider)
    assert capability_enabled(caps.folding_range_provider)
    assert capability_enabled(caps.inlay_hint_provider)
    assert capability_enabled(caps.code_action_provider)
    assert caps.semantic_tokens_provider is not None


@pytest.mark.workspace("hello_world")
async def test_semantic_token_modifier_legend(client, workspace):
    legend = client.init_result.capabilities.semantic_tokens_provider.legend
    assert legend is not None
    assert list(legend.token_modifiers) == [
        "declaration",
        "definition",
        "const",
        "overloaded",
        "typed",
        "templated",
        "deprecated",
        "deduced",
        "readonly",
        "static",
        "abstract",
        "virtual",
        "dependentName",
        "defaultLibrary",
        "usedAsMutableReference",
        "usedAsMutablePointer",
        "constructorOrDestructor",
        "userDefined",
        "functionScope",
        "classScope",
        "fileScope",
        "globalScope",
    ]


@pytest.mark.workspace("hello_world")
async def test_did_open_close_cycle(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(SETTLE_TIME)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_shutdown_exit(client, workspace):
    await client.shutdown_async(None)


@pytest.mark.workspace("hello_world")
async def test_feature_requests_after_close(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    client.close(uri)
    with pytest.raises(Exception, match="Document not open"):
        await client.hover_at(uri, 0, 0)


@pytest.mark.workspace("hello_world")
async def test_incremental_change(client, workspace):
    uri, content = client.open(workspace / "main.cpp")
    for i in range(5):
        content += f"\n// change {i}"
        did_change(client, uri, i + 1, content)
        await asyncio.sleep(0.05)
    await asyncio.sleep(SETTLE_TIME * 2)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_diagnostics_received(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert uri in client.diagnostics
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_hover_before_compile(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    result = await client.hover_at(uri, 0, 0, timeout=90.0)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_completion_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.completion_at(uri, 0, 0)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_signature_help_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.signature_help_at(uri, 0, 0)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_definition_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.definition_at(uri, 2, 4)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_document_symbol_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.document_symbols(uri)
    assert result is not None
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_folding_range_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.folding_ranges(uri)
    assert result is not None
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_semantic_tokens_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.semantic_tokens_full(uri)
    assert result is not None
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_inlay_hint_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.inlay_hints(
        uri,
        Range(start=Position(line=0, character=0), end=Position(line=10, character=0)),
    )
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_code_action_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.code_actions(
        uri,
        Range(start=Position(line=0, character=0), end=Position(line=0, character=10)),
    )
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_document_link_request(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    result = await client.document_links(uri)
    assert result is not None
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_rapid_changes_stress(client, workspace):
    uri, content = client.open(workspace / "main.cpp")
    for i in range(20):
        content += f"\n// stress change {i}\n"
        did_change(client, uri, i + 1, content)
    await asyncio.sleep(SETTLE_TIME * 2)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_save_notification(client, workspace):
    uri, _ = client.open(workspace / "main.cpp")
    await asyncio.sleep(SETTLE_TIME)
    client.text_document_did_save(DidSaveTextDocumentParams(text_document=doc(uri)))
    await asyncio.sleep(SETTLE_TIME)
    client.close(uri)


@pytest.mark.workspace("hello_world")
async def test_hover_on_unknown_file(client, workspace):
    with pytest.raises(Exception, match="Document not open"):
        await client.hover_at("file:///nonexistent/fake.cpp", 0, 0)


@pytest.mark.workspace("hello_world")
async def test_all_features_after_compile_wait(client, workspace):
    """Exercise all feature requests after compilation completes."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    hover = await client.hover_at(uri, 2, 4)
    assert hover is not None

    completion = await client.completion_at(uri, 7, 18)

    await client.signature_help_at(uri, 0, 0)

    await client.definition_at(uri, 2, 4)

    symbols = await client.document_symbols(uri)
    assert symbols is not None

    folding = await client.folding_ranges(uri)
    assert folding is not None

    tokens = await client.semantic_tokens_full(uri)
    assert tokens is not None

    links = await client.document_links(uri)
    assert links is not None

    await client.code_actions(
        uri,
        Range(start=Position(line=0, character=0), end=Position(line=0, character=10)),
    )

    await client.inlay_hints(
        uri,
        Range(start=Position(line=0, character=0), end=Position(line=10, character=0)),
    )

    client.close(uri)
