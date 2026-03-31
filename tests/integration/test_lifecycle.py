"""Lifecycle tests for the clice LSP server using pygls."""

from pathlib import Path

import pytest
from conftest import CliceClient
from lsprotocol.types import ClientCapabilities, InitializeParams


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_initialize(client: CliceClient, workspace: Path):
    assert client.init_result is not None
    assert client.init_result.server_info is not None
    assert client.init_result.server_info.name == "clice"


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_double_initialize_rejected(client: CliceClient, workspace: Path):
    with pytest.raises(Exception):
        await client.initialize_async(
            InitializeParams(
                capabilities=ClientCapabilities(),
                workspace_folders=[],
            )
        )


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_shutdown(client: CliceClient, workspace: Path):
    await client.shutdown_async(None)
