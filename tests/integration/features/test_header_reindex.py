"""Saving a header must reindex the closed TUs that include it."""

import asyncio

import pytest
from lsprotocol.types import (
    DidChangeTextDocumentParams,
    DidSaveTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    TextDocumentIdentifier,
    VersionedTextDocumentIdentifier,
)

from tests.conftest import make_client
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


async def test_header_save_reindexes_dependents(executable, tmp_path):
    (tmp_path / "header.h").write_text(HEADER_V1)
    (tmp_path / "closed.cpp").write_text(CLOSED_TU)
    write_cdb(tmp_path, ["closed.cpp"])

    # Own the client so the full server log can be dumped on failure
    # (the shared fixture only surfaces warnings and errors).
    client = await make_client(executable, tmp_path)
    try:
        header_uri = (tmp_path / "header.h").as_uri()
        closed_uri = (tmp_path / "closed.cpp").as_uri()
        await client.open_and_wait(tmp_path / "header.h")

        failures = []
        # Initial background index: the closed TU's call resolves to alpha.
        if not await wait_for_reference(client, header_uri, 1, 11, closed_uri):
            failures.append("initial index never produced the alpha reference")
        elif closed_uri in await reference_uris(client, header_uri, 2, 11):
            failures.append("beta unexpectedly referenced before the save")
        else:
            await asyncio.sleep(MTIME_GRANULARITY)
            (tmp_path / "header.h").write_text(HEADER_V2)
            client.text_document_did_change(
                DidChangeTextDocumentParams(
                    text_document=VersionedTextDocumentIdentifier(
                        uri=header_uri, version=2
                    ),
                    content_changes=[
                        TextDocumentContentChangeWholeDocument(text=HEADER_V2)
                    ],
                )
            )
            client.text_document_did_save(
                DidSaveTextDocumentParams(
                    text_document=TextDocumentIdentifier(uri=header_uri)
                )
            )

            # The closed TU is reindexed against the saved header: its call
            # now references beta, and the stale alpha reference is gone.
            if not await wait_for_reference(client, header_uri, 2, 11, closed_uri):
                failures.append("closed TU was not reindexed after the header save")
            elif closed_uri in await reference_uris(client, header_uri, 1, 11):
                failures.append("stale alpha reference still served")
    finally:
        server = client.server
        try:
            await asyncio.wait_for(client.shutdown_async(None), timeout=10.0)
            client.exit(None)
        except Exception:
            client.kill_server()
        try:
            await asyncio.wait_for(server.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            server.kill()
            await server.wait()

    if failures:
        tail = ""
        if server.stderr:
            data = await asyncio.wait_for(server.stderr.read(), timeout=5.0)
            text = data.decode("utf-8", errors="replace")
            tail = "\n".join(text.splitlines()[-150:])
        pytest.fail("\n".join(failures) + "\n--- server log tail ---\n" + tail)
