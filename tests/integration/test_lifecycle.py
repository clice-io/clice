"""Lifecycle tests for the clice LSP server using pygls."""

import pytest
from conftest import lsp_initialize


@pytest.mark.asyncio
async def test_initialize(client, test_data_dir):
    ws = test_data_dir / "hello_world"
    result = await lsp_initialize(client, ws)
    assert result.server_info is not None
    assert result.server_info.name == "clice"


@pytest.mark.asyncio
async def test_shutdown(client, test_data_dir):
    ws = test_data_dir / "hello_world"
    await lsp_initialize(client, ws)
    await client.shutdown_async(None)
