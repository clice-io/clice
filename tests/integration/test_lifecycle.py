"""Lifecycle tests for the clice LSP server using pygls."""

import pytest


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_initialize(client, ws):
    result = await client.initialize(ws)
    assert result.server_info is not None
    assert result.server_info.name == "clice"


@pytest.mark.asyncio
@pytest.mark.workspace("hello_world")
async def test_shutdown(client, ws):
    await client.initialize(ws)
    await client.shutdown_async(None)
