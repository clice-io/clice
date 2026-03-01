import pytest
import asyncio
from tests.fixtures.client import LSPClient

DOT_COMPLETION_CASE = """#include <vector>

int add(int lhs, int rhs) {
    return lhs + rhs;
}

int main() {
    std::vector<int> values;
    values.
    auto n = add(1, 2);
    return n;
}
"""


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


@pytest.mark.asyncio
async def test_hover_save_close(client: LSPClient, test_data_dir):
    workspace = test_data_dir / "feature_requests"
    await client.initialize(workspace)
    await client.did_open("main.cpp")

    content = client.get_file("main.cpp").content + "\nint saved = 1;\n"
    await client.did_change("main.cpp", content)
    await client.did_save("main.cpp", include_text=True)

    hover = await client.hover("main.cpp", 0, 0)
    assert hover is not None
    assert "contents" in hover
    assert "value" in hover["contents"]
    assert "main.cpp" in hover["contents"]["value"]

    completion = await client.completion("main.cpp", 8, 10)
    assert completion is not None
    assert "items" in completion
    assert len(completion["items"]) > 0
    assert "label" in completion["items"][0]

    signature_help = await client.signature_help("main.cpp", 7, 17)
    assert signature_help is not None
    assert "signatures" in signature_help
    assert len(signature_help["signatures"]) > 0
    assert "label" in signature_help["signatures"][0]

    # Cancellation for unknown requests should not affect normal requests.
    await client.send_notification("$/cancelRequest", {"id": 99999})
    await client.did_close("main.cpp")

    closed_hover = await client.send_request(
        "textDocument/hover",
        {
            "textDocument": {"uri": (workspace / "main.cpp").as_uri()},
            "position": {"line": 0, "character": 0},
        },
    )
    assert closed_hover is None


@pytest.mark.asyncio
async def test_completion_after_dot_does_not_close_worker(
    client: LSPClient, test_data_dir
):
    workspace = test_data_dir / "feature_requests"
    await client.initialize(workspace)
    await client.did_open("main.cpp")
    await client.did_change("main.cpp", DOT_COMPLETION_CASE)
    await client.did_save("main.cpp", include_text=True)

    completion = await client.completion("main.cpp", 8, 11)
    assert completion is not None
    assert "items" in completion
    assert len(completion["items"]) > 0

    hover = await client.hover("main.cpp", 0, 0)
    assert hover is not None
    assert "contents" in hover
