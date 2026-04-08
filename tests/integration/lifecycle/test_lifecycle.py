"""Lifecycle tests for the clice LSP server."""

import pytest
from lsprotocol.types import ClientCapabilities, InitializeParams


@pytest.mark.workspace("hello_world")
async def test_initialize(client, workspace):
    assert client.init_result is not None
    assert client.init_result.server_info is not None
    assert client.init_result.server_info.name == "clice"


@pytest.mark.workspace("hello_world")
async def test_double_initialize_rejected(client, workspace):
    with pytest.raises(Exception):
        await client.initialize_async(
            InitializeParams(
                capabilities=ClientCapabilities(),
                workspace_folders=[],
            )
        )


@pytest.mark.workspace("hello_world")
async def test_shutdown(client, workspace):
    await client.shutdown_async(None)
