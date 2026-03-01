import pytest
from tests.fixtures.client import LSPClient
from tests.fixtures.transport import LSPError


@pytest.mark.asyncio
async def test_initialize(client: LSPClient, test_data_dir):
    result = await client.initialize(test_data_dir)
    assert "serverInfo" in result
    assert result["serverInfo"]["name"] == "clice"


@pytest.mark.asyncio
async def test_shutdown_rejects_feature_requests(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "hello_world")
    await client.did_open("main.cpp")
    await client.shutdown()

    with pytest.raises(LSPError):
        await client.hover("main.cpp", 0, 0)

    with pytest.raises(LSPError):
        await client.completion("main.cpp", 0, 0)

    with pytest.raises(LSPError):
        await client.signature_help("main.cpp", 0, 0)
