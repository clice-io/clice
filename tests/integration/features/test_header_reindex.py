"""Saving a header must reindex the closed TUs that include it."""

import asyncio

from lsprotocol.types import (
    DidChangeTextDocumentParams,
    DidSaveTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)

from tests.integration.utils import write_cdb
from tests.integration.utils.wait import MTIME_GRANULARITY

HEADER_V1 = """\
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
"""

# Retargets the closed TU's call from alpha() to beta() without touching
# the TU itself — only a reindex against the new header can see it.
HEADER_V2 = """\
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
"""

CLOSED_TU = '#include "header.h"\nint use_target() { return TARGET(); }\n'


async def reference_uris(client, uri, line, character):
    refs = await client.references_at(uri, line, character, include_declaration=False)
    return [ref.uri for ref in (refs or [])]


async def wait_for_reference(client, uri, line, character, expected_uri, timeout=30):
    for _ in range(timeout):
        if expected_uri in await reference_uris(client, uri, line, character):
            return True
        await asyncio.sleep(1)
    return False


async def test_header_save_reindexes_dependents(client, tmp_path):
    # newline="\n" keeps the on-disk bytes identical to the didChange text
    # below: after a save the buffer and the disk must agree, as they do for
    # a real editor. Index navigation on an open file resolves positions
    # against the buffer while shards index the disk content, so a CRLF
    # translation here would make every lookup miss on Windows.
    (tmp_path / "header.h").write_text(HEADER_V1, newline="\n")
    (tmp_path / "closed.cpp").write_text(CLOSED_TU, newline="\n")
    write_cdb(tmp_path, ["closed.cpp"])
    await client.initialize(tmp_path)

    header_uri = (tmp_path / "header.h").as_uri()
    closed_uri = (tmp_path / "closed.cpp").as_uri()
    await client.open_and_wait(tmp_path / "header.h")

    # Initial background index: the closed TU's call resolves to alpha.
    assert await wait_for_reference(client, header_uri, 1, 11, closed_uri), (
        "initial index never produced the closed TU's alpha reference"
    )
    assert closed_uri not in await reference_uris(client, header_uri, 2, 11)

    await asyncio.sleep(MTIME_GRANULARITY)
    (tmp_path / "header.h").write_text(HEADER_V2, newline="\n")
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=header_uri, version=2),
            content_changes=[TextDocumentContentChangeWholeDocument(text=HEADER_V2)],
        )
    )
    client.text_document_did_save(
        DidSaveTextDocumentParams(text_document=TextDocumentIdentifier(uri=header_uri))
    )

    # The closed TU is reindexed against the saved header: its call now
    # references beta, and the stale alpha reference is gone.
    assert await wait_for_reference(client, header_uri, 2, 11, closed_uri), (
        "closed TU was not reindexed after the header save"
    )
    assert closed_uri not in await reference_uris(client, header_uri, 1, 11)
