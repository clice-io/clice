"""Saving a module interface must reindex the closed TUs that import it."""

import asyncio

from lsprotocol.types import (
    DidChangeTextDocumentParams,
    DidSaveTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)

from tests.integration.utils import write_cdb
from tests.integration.utils.wait import (
    MTIME_GRANULARITY,
    wait_for_recompile,
    wait_for_reference,
)

INTERFACE_V1 = """\
export module mod;
export inline int pick() { return 1; }
"""

# Changes pick's signature (and thus its symbol identity) while keeping the
# closed importer's call valid and the declaration at the same position —
# only a reindex against the new interface can retarget the call.
INTERFACE_V2 = """\
export module mod;
export inline int pick(int x = 0) { return 2; }
"""

CLOSED_IMPORTER = "import mod;\nint run() { return pick(); }\n"


async def test_interface_save_reindexes_importers(client, tmp_path):
    (tmp_path / "mod.cppm").write_text(INTERFACE_V1, newline="\n")
    (tmp_path / "main.cpp").write_text(CLOSED_IMPORTER, newline="\n")
    write_cdb(tmp_path, ["main.cpp", "mod.cppm"], std="c++20")
    await client.initialize(tmp_path)

    mod_uri = (tmp_path / "mod.cppm").as_uri()
    main_uri = (tmp_path / "main.cpp").as_uri()
    await client.open_and_wait(tmp_path / "mod.cppm")

    # Initial background index: the closed importer's call resolves to pick.
    assert await wait_for_reference(client, mod_uri, 1, 18, main_uri), (
        "initial index never produced the closed importer's pick reference"
    )

    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "mod.cppm").write_text(INTERFACE_V2, newline="\n")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=mod_uri, version=2),
            content_changes=[TextDocumentContentChangeWholeDocument(text=INTERFACE_V2)],
        )
    )
    client.text_document_did_save(
        DidSaveTextDocumentParams(text_document=TextDocumentIdentifier(uri=mod_uri))
    )

    # References resolve against the open interface's compiled index.
    await wait_for_recompile(client, mod_uri)

    # The closed importer is reindexed against the saved interface: its call
    # now targets the new pick signature, which only a rebuilt shard can know.
    assert await wait_for_reference(client, mod_uri, 1, 18, main_uri), (
        "closed importer was not reindexed after the interface save"
    )
