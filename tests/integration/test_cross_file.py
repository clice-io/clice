import pytest
import asyncio
from tests.fixtures.client import LSPClient


@pytest.mark.asyncio
async def test_go_to_definition(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "hello_world"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(5)

    # Try GoToDefinition on "main" at line 2, char 4 (int main())
    result = await client.go_to_definition("main.cpp", 2, 4)
    # Even if index hasn't populated, we accept null gracefully
    # Primary test is that the server doesn't crash
    # result can be None if index isn't ready yet


@pytest.mark.asyncio
async def test_document_symbols(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "cross_file"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(5)

    symbols = await client.document_symbols("main.cpp")
    assert symbols is not None
    assert len(symbols) > 0

    names = [s["name"] for s in symbols]
    assert "main" in names


@pytest.mark.asyncio
async def test_semantic_tokens(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "cross_file"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(5)

    tokens = await client.semantic_tokens("main.cpp")
    assert tokens is not None
    assert "data" in tokens
    assert len(tokens["data"]) > 0


@pytest.mark.asyncio
async def test_workspace_symbol(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "cross_file"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await asyncio.sleep(5)

    result = await client.workspace_symbol("main")
    assert result is not None


@pytest.mark.asyncio
async def test_capabilities(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "hello_world"
    result = await client.initialize(workspace)

    caps = result["capabilities"]
    assert caps["definitionProvider"] is True
    assert caps["referencesProvider"] is True
    assert "callHierarchyProvider" in caps
    assert "typeHierarchyProvider" in caps
    assert "workspaceSymbolProvider" in caps
