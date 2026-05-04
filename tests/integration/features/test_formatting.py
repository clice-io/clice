import pytest
from lsprotocol.types import Position, Range

from tests.integration.utils.workspace import did_change

UNFORMATTED = "int    add(   int   a  ,  int   b  ) {\nreturn   a+b ;\n}\n"
FORMATTED = "int add(int a, int b) { return a + b; }\n"


@pytest.mark.workspace("formatting")
async def test_format_document(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, UNFORMATTED)
    edits = await client.format_document(uri)

    assert edits is not None
    assert len(edits) > 0

    client.close(uri)


@pytest.mark.workspace("formatting")
async def test_format_range(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, UNFORMATTED)
    edits = await client.format_range(
        uri,
        Range(start=Position(line=1, character=0), end=Position(line=2, character=0)),
    )

    assert edits is not None
    assert len(edits) > 0

    client.close(uri)


@pytest.mark.workspace("formatting")
async def test_format_already_formatted(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, FORMATTED)
    edits = await client.format_document(uri)

    assert edits is not None
    assert len(edits) == 0

    client.close(uri)
