import pytest
from tests.fixtures.client import LSPClient


@pytest.mark.asyncio
async def test_initialize(client: LSPClient, test_data_dir):
    result = await client.initialize(test_data_dir)
    assert "serverInfo" in result
    assert result["serverInfo"]["name"] == "clice"
