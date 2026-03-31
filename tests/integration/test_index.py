"""Integration tests for index-based LSP features: GoToDefinition, FindReferences,
CallHierarchy, TypeHierarchy, and WorkspaceSymbol."""

import asyncio

import pytest
from lsprotocol.types import (
    CallHierarchyIncomingCallsParams,
    CallHierarchyOutgoingCallsParams,
    CallHierarchyPrepareParams,
    DefinitionParams,
    DidCloseTextDocumentParams,
    Position,
    ReferenceContext,
    ReferenceParams,
    TextDocumentIdentifier,
    TypeHierarchyPrepareParams,
    TypeHierarchySubtypesParams,
    TypeHierarchySupertypesParams,
    WorkspaceSymbolParams,
)


def _doc(uri: str) -> TextDocumentIdentifier:
    return TextDocumentIdentifier(uri=uri)


# ---------------------------------------------------------------------------
# GoToDefinition
# ---------------------------------------------------------------------------


@pytest.mark.workspace("index_features")
async def test_goto_definition(client, workspace):
    """Test GoToDefinition navigates from a call site to the function definition."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)  # Wait for background indexing

    # 'add' call on line 23 (0-indexed), column 12
    result = await client.text_document_definition_async(
        DefinitionParams(
            text_document=_doc(uri),
            position=Position(line=23, character=12),
        )
    )
    assert result is not None
    locs = result if isinstance(result, list) else [result]
    assert len(locs) > 0
    # Definition should point to line 17 where 'int add(...)' is declared
    assert any(loc.range.start.line == 17 for loc in locs)

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# FindReferences
# ---------------------------------------------------------------------------


@pytest.mark.workspace("index_features")
async def test_find_references(client, workspace):
    """Test FindReferences returns all usages of global_var."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # global_var definition on line 29, column 4
    result = await client.text_document_references_async(
        ReferenceParams(
            text_document=_doc(uri),
            position=Position(line=29, character=4),
            context=ReferenceContext(include_declaration=True),
        )
    )
    assert result is not None
    # global_var is declared on line 29 and used on lines 32 and 36
    assert len(result) >= 3

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# CallHierarchy
# ---------------------------------------------------------------------------


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_prepare(client, workspace):
    """Test prepareCallHierarchy returns a CallHierarchyItem for 'add'."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # 'add' definition at line 17, column 4
    result = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=17, character=4),
        )
    )
    assert result is not None
    assert len(result) > 0
    assert result[0].name == "add"

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_incoming(client, workspace):
    """Test incomingCalls shows compute() calls add()."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # Prepare call hierarchy for 'add' at line 17, column 4
    items = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=17, character=4),
        )
    )
    assert items and len(items) > 0

    incoming = await client.call_hierarchy_incoming_calls_async(
        CallHierarchyIncomingCallsParams(item=items[0])
    )
    assert incoming is not None
    caller_names = [call.from_.name for call in incoming]
    assert "compute" in caller_names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_outgoing(client, workspace):
    """Test outgoingCalls shows compute() calls add()."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # Prepare call hierarchy for 'compute' at line 22, column 4
    items = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=22, character=4),
        )
    )
    assert items and len(items) > 0

    outgoing = await client.call_hierarchy_outgoing_calls_async(
        CallHierarchyOutgoingCallsParams(item=items[0])
    )
    assert outgoing is not None
    callee_names = [call.to.name for call in outgoing]
    assert "add" in callee_names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# TypeHierarchy
# ---------------------------------------------------------------------------


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_prepare(client, workspace):
    """Test prepareTypeHierarchy returns a TypeHierarchyItem for 'Dog'."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # 'Dog' at line 7, column 7
    result = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=7, character=7),
        )
    )
    assert result is not None
    assert len(result) > 0
    assert result[0].name == "Dog"

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_supertypes(client, workspace):
    """Test supertypes of Dog includes Animal."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # 'Dog' at line 7, column 7
    items = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=7, character=7),
        )
    )
    assert items and len(items) > 0

    supertypes = await client.type_hierarchy_supertypes_async(
        TypeHierarchySupertypesParams(item=items[0])
    )
    assert supertypes is not None
    supertype_names = [t.name for t in supertypes]
    assert "Animal" in supertype_names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_subtypes(client, workspace):
    """Test subtypes of Animal includes Dog and Cat."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    # 'Animal' at line 1, column 7
    items = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=_doc(uri),
            position=Position(line=1, character=7),
        )
    )
    assert items and len(items) > 0

    subtypes = await client.type_hierarchy_subtypes_async(
        TypeHierarchySubtypesParams(item=items[0])
    )
    assert subtypes is not None
    subtype_names = [t.name for t in subtypes]
    assert "Dog" in subtype_names
    assert "Cat" in subtype_names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


# ---------------------------------------------------------------------------
# WorkspaceSymbol
# ---------------------------------------------------------------------------


@pytest.mark.workspace("index_features")
async def test_workspace_symbol(client, workspace):
    """Test workspace/symbol finds symbols by query string."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    result = await client.workspace_symbol_async(WorkspaceSymbolParams(query="add"))
    assert result is not None
    names = [s.name for s in result]
    assert "add" in names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))


@pytest.mark.workspace("index_features")
async def test_workspace_symbol_class(client, workspace):
    """Test workspace/symbol finds class symbols."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    await asyncio.sleep(15)

    result = await client.workspace_symbol_async(WorkspaceSymbolParams(query="Animal"))
    assert result is not None
    names = [s.name for s in result]
    assert "Animal" in names

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=_doc(uri)))
