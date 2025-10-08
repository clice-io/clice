import pytest
import asyncio
from tests.fixtures.client import LSPClient


@pytest.mark.asyncio
async def test_did_open(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "hello_world")
    await client.did_open("main.cpp")
    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_did_change(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "hello_world")
    await client.did_open("main.cpp")

    # Test frequently change content will not make server crash.
    content = client.get_file("main.cpp").content

    for _ in range(0, 20):
        content += "\n"
        await asyncio.sleep(0.2)
        await client.did_change("main.cpp", content)

    await asyncio.sleep(5)


@pytest.mark.asyncio
async def test_clang_tidy(client: LSPClient, test_data_dir):
    await client.initialize(test_data_dir / "clang_tidy")
    await client.did_open("main.cpp")
    await asyncio.sleep(5)
