"""Navigation on a freshly edited buffer: the bounded compile wait must let
the fresh file index answer instead of silently using pre-edit shards."""

import asyncio

import pytest

from tests.integration.utils.workspace import did_change


@pytest.mark.workspace("hello_world")
async def test_definition_after_edit(client, workspace):
    uri, content = await client.open_and_wait(workspace / "main.cpp")

    # Prepend two lines: only the freshly compiled index can produce the
    # moved definition location; a stale shard answer is empty here.
    did_change(client, uri, 2, "// one\n// two\n" + content)

    # A slow runner may see the degraded (empty) answer; retry until the
    # compile settles.
    locations = []
    for _ in range(60):
        result = await client.definition_at(uri, 11, 17)
        locations = result if isinstance(result, list) else [result] if result else []
        if locations:
            break
        await asyncio.sleep(0.3)
    assert locations, "definition never resolved"

    assert all(loc.range.start.line == 4 for loc in locations), (
        f"expected the post-edit definition line 4, got"
        f" {[(loc.uri, loc.range.start.line) for loc in locations]}"
    )
