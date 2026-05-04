import pytest
from lsprotocol.types import Position, Range

from tests.integration.utils.workspace import did_change

UNFORMATTED = "int    add(   int   a  ,  int   b  ) {\nreturn   a+b ;\n}\n"
FORMATTED = "int add(int a, int b) { return a + b; }\n"


def apply_edits(text, edits):
    """Apply LSP TextEdits to a string, processing from end to start."""
    lines = text.split("\n")
    for edit in sorted(
        edits, key=lambda e: (e.range.start.line, e.range.start.character), reverse=True
    ):
        start = edit.range.start
        end = edit.range.end
        before = (
            "\n".join(lines[: start.line])
            + ("\n" if start.line > 0 else "")
            + lines[start.line][: start.character]
        )
        after = (
            lines[end.line][end.character :]
            + ("\n" if end.line < len(lines) - 1 else "")
            + "\n".join(lines[end.line + 1 :])
        )
        text = before + edit.new_text + after
        lines = text.split("\n")
    return text


@pytest.mark.workspace("formatting")
async def test_format_document(client, workspace):
    uri, _ = await client.open_and_wait(workspace / "main.cpp")

    did_change(client, uri, 1, UNFORMATTED)
    edits = await client.format_document(uri)

    assert edits is not None
    assert len(edits) > 0
    result = apply_edits(UNFORMATTED, edits)
    assert result == FORMATTED

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
