"""Navigation right after an edit must resolve against the edited buffer:
the server settles the file's compile before answering, with no timeout."""

from tests.tools.compile_commands import write_cdb
from tests.tools.workspace import did_change

SOURCE_V1 = "int foo() { return 1; }\nint main() { return foo(); }\n"

# Inserts a line at the top: every position shifts, so a query resolved
# against the pre-edit index would name the wrong symbol or nothing.
SOURCE_V2 = "// shift\nint foo() { return 1; }\nint main() { return foo(); }\n"


async def test_navigation_after_change(client, tmp_path):
    (tmp_path / "main.cpp").write_text(SOURCE_V1, newline="\n")
    write_cdb(tmp_path, ["main.cpp"])
    await client.initialize(tmp_path)

    uri, _ = await client.open_and_wait(tmp_path / "main.cpp")

    # No wait after the edit: the requests below must settle the compile
    # themselves before resolving the cursor.
    did_change(client, uri, 2, SOURCE_V2)

    defs = await client.definition_at(uri, 2, 20)
    assert defs, "definition right after an edit returned nothing"
    assert defs[0].range.start.line == 1

    refs = await client.references_at(uri, 2, 20, include_declaration=False)
    assert refs, "references right after an edit returned nothing"
    assert {loc.range.start.line for loc in refs} == {2}
