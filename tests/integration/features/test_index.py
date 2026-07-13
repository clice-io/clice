"""Integration tests for index-based LSP features: GoToDefinition, FindReferences,
CallHierarchy, TypeHierarchy, and WorkspaceSymbol."""

import pytest
from lsprotocol.types import (
    CallHierarchyIncomingCallsParams,
    CallHierarchyItem,
    CallHierarchyOutgoingCallsParams,
    CallHierarchyPrepareParams,
    Position,
    Range,
    SymbolKind,
    TypeHierarchyPrepareParams,
    TypeHierarchySubtypesParams,
    TypeHierarchySupertypesParams,
    WorkspaceSymbolParams,
)

from tests.tools.workspace import doc
from tests.tools.checks import locations_of, wait_for_index


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
async def test_call_hierarchy_bogus_item(client, workspace):
    """incomingCalls with an unresolvable item returns an error, not null."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    bogus = CallHierarchyItem(
        name="ghost",
        kind=SymbolKind.Function,
        uri="file:///nonexistent/ghost.cpp",
        range=Range(start=Position(0, 0), end=Position(0, 5)),
        selection_range=Range(start=Position(0, 0), end=Position(0, 5)),
    )
    with pytest.raises(Exception, match="Failed to resolve call hierarchy item"):
        await client.call_hierarchy_incoming_calls_async(
            CallHierarchyIncomingCallsParams(item=bogus)
        )
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


@pytest.mark.workspace("index_features")
async def test_goto_declaration_cross_file(client, workspace):
    # Query a closed file: background indexing skips open files, so nav.h's
    # shard only exists while nav.cpp stays closed (recorded index gap).
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_uri = (workspace / "nav.cpp").as_uri()

    # 'area' definition in nav.cpp line 2; declaration in nav.h line 4.
    locs = locations_of(await client.declaration_at(nav_uri, 2, 4))
    lines = [(loc.uri.split("/")[-1], loc.range.start.line) for loc in locs]
    assert ("nav.h", 4) in lines, f"expected nav.h:4 declaration, got {lines}"
    # Union semantics: the definition itself is also listed.
    assert ("nav.cpp", 2) in lines, f"expected nav.cpp:2 definition, got {lines}"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_goto_declaration_inline_definition(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'add' (line 18) is defined inline with no separate declaration;
    # declaration must still navigate to the definition, not return empty.
    locs = locations_of(await client.declaration_at(uri, 18, 4))
    assert any(loc.range.start.line == 18 for loc in locs), (
        f"expected the definition at line 18, got"
        f" {[(loc.uri, loc.range.start.line) for loc in locs]}"
    )

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_goto_implementation(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # Animal::speak (line 2) is overridden by Dog::speak (9) and Cat::speak (14).
    locs = locations_of(await client.implementation_at(uri, 2, 17))
    lines = sorted(loc.range.start.line for loc in locs)
    assert lines == [9, 14], f"expected overrides at lines 9 and 14, got {lines}"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_goto_type_definition(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_uri = (workspace / "nav.cpp").as_uri()

    # global_shape (line 6) has type Shape, defined in nav.h line 6.
    locs = locations_of(await client.type_definition_at(nav_uri, 6, 6))
    lines = [(loc.uri.split("/")[-1], loc.range.start.line) for loc in locs]
    assert ("nav.h", 6) in lines, f"expected Shape definition nav.h:6, got {lines}"

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_references_declaration_flag(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_uri = (workspace / "nav.cpp").as_uri()

    # 'area': declaration nav.h:4, definition nav.cpp:2, call nav.cpp:9.
    with_decl = locations_of(await client.references_at(nav_uri, 2, 4))
    without_decl = locations_of(
        await client.references_at(nav_uri, 2, 4, include_declaration=False)
    )
    assert len(with_decl) == 3, with_decl
    with_set = {(loc.uri.split("/")[-1], loc.range.start.line) for loc in with_decl}
    without_set = {
        (loc.uri.split("/")[-1], loc.range.start.line) for loc in without_decl
    }
    assert with_set == {("nav.h", 4), ("nav.cpp", 2), ("nav.cpp", 9)}, with_set
    assert without_set == {("nav.cpp", 9)}, without_set

    client.close(uri)


@pytest.mark.workspace("index_features")
async def test_goto_implementation_pure_virtual(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_h = (workspace / "nav.h").as_uri()

    # Renderer::render is pure virtual; only the direct override's definition
    # is returned (DebugGLRenderer::render belongs to GLRenderer::render).
    locs = locations_of(await client.implementation_at(nav_h, 12, 17))
    lines = [(loc.uri.split("/")[-1], loc.range.start.line) for loc in locs]
    assert lines == [("nav.cpp", 12)], lines


@pytest.mark.workspace("index_features")
async def test_goto_implementation_chain(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_h = (workspace / "nav.h").as_uri()

    # Intermediate override navigates to its own overriders.
    locs = locations_of(await client.implementation_at(nav_h, 18, 9))
    lines = [(loc.uri.split("/")[-1], loc.range.start.line) for loc in locs]
    assert lines == [("nav.cpp", 14)], lines


@pytest.mark.workspace("index_features")
async def test_goto_type_definition_return_value(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_h = (workspace / "nav.h").as_uri()

    # Known index gap: functions carry no TypeDefinition relation for their
    # return type, so this currently yields no results.
    result = await client.type_definition_at(nav_h, 25, 6)
    assert result is not None and len(result) == 0, result


@pytest.mark.workspace("index_features")
async def test_goto_declaration_forward_declared(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_uri = (workspace / "nav.cpp").as_uri()

    # 'Shape' at the global_shape declaration: forward declaration and
    # definition are both listed.
    locs = locations_of(await client.declaration_at(nav_uri, 6, 0))
    lines = [(loc.uri.split("/")[-1], loc.range.start.line) for loc in locs]
    assert ("nav.h", 2) in lines, lines
    assert ("nav.h", 6) in lines, lines


@pytest.mark.workspace("index_features")
async def test_navigation_empty_open_document(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri), "Index not ready after 30s"

    # 'add' has no implementations: open documents get [] back, not an error.
    result = await client.implementation_at(uri, 18, 4)
    assert result is not None and len(result) == 0, result


@pytest.mark.workspace("index_features")
async def test_navigation_closed_document_empty(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    assert await wait_for_index(client, uri, "area"), "Index not ready after 30s"
    nav_uri = (workspace / "nav.cpp").as_uri()

    # Index-only navigation serves closed documents; an empty result is a
    # real answer, not an error.
    result = await client.implementation_at(nav_uri, 2, 4)
    assert result is not None and len(result) == 0, result


@pytest.mark.workspace("index_features")
async def test_definition_on_include(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "nav.cpp")

    # Preamble include (line 0): served master-side from the PCH's links.
    locs = locations_of(await client.definition_at(uri, 0, 12))
    assert any(loc.uri.endswith("nav.h") for loc in locs), locs

    # Non-preamble include (line 20): served by the worker's AST.
    locs = locations_of(await client.definition_at(uri, 20, 12))
    assert any(loc.uri.endswith("nav_late.h") for loc in locs), locs


@pytest.mark.workspace("index_features")
async def test_document_links_include_preamble(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "nav.cpp")
    links = await client.document_links(uri)
    targets = [link.target or "" for link in links or []]
    assert any("nav.h" in target for target in targets), targets
    assert any("nav_late.h" in target for target in targets), targets
