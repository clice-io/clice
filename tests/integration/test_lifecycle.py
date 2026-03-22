"""Lifecycle tests for the clice LSP server using pygls."""

import pytest
from lsprotocol.types import (
    ClientCapabilities,
    InitializeParams,
    InitializedParams,
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
async def test_shutdown(client, test_data_dir):
    ws = test_data_dir / "hello_world"
    await client.initialize_async(
        InitializeParams(
            capabilities=ClientCapabilities(),
            root_uri=ws.as_uri(),
            workspace_folders=[WorkspaceFolder(uri=ws.as_uri(), name="test")],
        )
    )
    client.initialized(InitializedParams())
    await client.shutdown_async(None)
