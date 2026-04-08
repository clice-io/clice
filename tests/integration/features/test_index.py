"""Integration tests for index-based LSP features: GoToDefinition, FindReferences,
CallHierarchy, TypeHierarchy, and WorkspaceSymbol."""

import pytest
from lsprotocol.types import (
    CallHierarchyIncomingCallsParams,
    CallHierarchyOutgoingCallsParams,
    CallHierarchyPrepareParams,
    Position,
    TypeHierarchyPrepareParams,
    TypeHierarchySubtypesParams,
    TypeHierarchySupertypesParams,
    WorkspaceSymbolParams,
)

from tests.integration.utils import doc
from tests.integration.utils.wait import wait_for_index


@pytest.mark.workspace("index_features")
async def test_goto_definition(client, workspace):
    """Test GoToDefinition navigates from a call site to the function definition."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'add' call on line 24 (0-indexed), column 12
    result = await client.definition_at(uri, 24, 12)
    assert result is not None
    locs = result if isinstance(result, list) else [result]
    assert len(locs) > 0, f"GoToDefinition returned empty list, result={result}"
    # Definition should point to line 18 where 'int add(...)' is declared
    assert any(loc.range.start.line == 18 for loc in locs), (
        f"Expected line 18, got locations:"
        f" {[(loc.uri, loc.range.start.line, loc.range.start.character) for loc in locs]}"
    )

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_find_references(client, workspace):
    """Test FindReferences returns all usages of global_var."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # global_var definition on line 30 (0-indexed), column 4
    result = await client.references_at(uri, 30, 4, include_declaration=True)
    assert result is not None, "FindReferences returned None"
    # global_var is declared on line 30 and used on lines 33 and 37
    assert len(result) >= 3, (
        f"Expected >=3 refs, got {len(result)}:"
        f" {[(r.uri, r.range.start.line) for r in result]}"
    )

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_prepare(client, workspace):
    """Test prepareCallHierarchy returns a CallHierarchyItem for 'add'."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'add' definition at line 18 (0-indexed), column 4
    result = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=18, character=4),
        )
    )
    assert result is not None, "prepareCallHierarchy returned None"
    assert len(result) > 0, f"prepareCallHierarchy returned empty, result={result}"
    assert result[0].name == "add", f"Expected 'add', got '{result[0].name}'"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_incoming(client, workspace):
    """Test incomingCalls shows compute() calls add()."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # Prepare call hierarchy for 'add' at line 18 (0-indexed), column 4
    items = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=18, character=4),
        )
    )
    assert items and len(items) > 0, f"prepareCallHierarchy returned {items}"

    incoming = await client.call_hierarchy_incoming_calls_async(
        CallHierarchyIncomingCallsParams(item=items[0])
    )
    assert incoming is not None, "incomingCalls returned None"
    caller_names = [call.from_.name for call in incoming]
    assert "compute" in caller_names, (
        f"Expected 'compute' in callers, got {caller_names}"
    )

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_call_hierarchy_outgoing(client, workspace):
    """Test outgoingCalls shows compute() calls add()."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # Prepare call hierarchy for 'compute' at line 23 (0-indexed), column 4
    items = await client.text_document_prepare_call_hierarchy_async(
        CallHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=23, character=4),
        )
    )
    assert items and len(items) > 0, f"prepareCallHierarchy returned {items}"

    outgoing = await client.call_hierarchy_outgoing_calls_async(
        CallHierarchyOutgoingCallsParams(item=items[0])
    )
    assert outgoing is not None, "outgoingCalls returned None"
    callee_names = [call.to.name for call in outgoing]
    assert "add" in callee_names, f"Expected 'add' in callees, got {callee_names}"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_prepare(client, workspace):
    """Test prepareTypeHierarchy returns a TypeHierarchyItem for 'Dog'."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'Dog' at line 8 (0-indexed), column 7
    result = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=8, character=7),
        )
    )
    assert result is not None, "prepareTypeHierarchy returned None"
    assert len(result) > 0, f"prepareTypeHierarchy returned empty"
    assert result[0].name == "Dog", f"Expected 'Dog', got '{result[0].name}'"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_supertypes(client, workspace):
    """Test supertypes of Dog includes Animal."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'Dog' at line 8 (0-indexed), column 7
    items = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=8, character=7),
        )
    )
    assert items and len(items) > 0, f"prepareTypeHierarchy returned {items}"

    supertypes = await client.type_hierarchy_supertypes_async(
        TypeHierarchySupertypesParams(item=items[0])
    )
    assert supertypes is not None, "supertypes returned None"
    supertype_names = [t.name for t in supertypes]
    assert "Animal" in supertype_names, (
        f"Expected 'Animal' in supertypes, got {supertype_names}"
    )

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_type_hierarchy_subtypes(client, workspace):
    """Test subtypes of Animal includes Dog and Cat."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'Animal' at line 1, column 7
    items = await client.text_document_prepare_type_hierarchy_async(
        TypeHierarchyPrepareParams(
            text_document=doc(uri),
            position=Position(line=1, character=7),
        )
    )
    assert items and len(items) > 0, f"prepareTypeHierarchy returned {items}"

    subtypes = await client.type_hierarchy_subtypes_async(
        TypeHierarchySubtypesParams(item=items[0])
    )
    assert subtypes is not None, "subtypes returned None"
    subtype_names = [t.name for t in subtypes]
    assert "Dog" in subtype_names, f"Expected 'Dog' in subtypes, got {subtype_names}"
    assert "Cat" in subtype_names, f"Expected 'Cat' in subtypes, got {subtype_names}"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_workspace_symbol(client, workspace):
    """Test workspace/symbol finds symbols by query string."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    result = await client.workspace_symbol_async(WorkspaceSymbolParams(query="add"))
    assert result is not None
    names = [s.name for s in result]
    assert "add" in names

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_workspace_symbol_class(client, workspace):
    """Test workspace/symbol finds class symbols."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    result = await client.workspace_symbol_async(WorkspaceSymbolParams(query="Animal"))
    assert result is not None
    names = [s.name for s in result]
    assert "Animal" in names

    client.close(uri)
