"""A poison document must not kill the session (F19 quarantine).

A file whose compiles crash the stateful worker is quarantined after two
consecutive crashes: healthy documents keep answering through it, the poison
file carries an explanatory diagnostic instead of an empty list, and an edit
that fixes it earns a probe that brings it back.
"""

import asyncio

import pytest
from lsprotocol.types import (
    DidChangeTextDocumentParams,
    TextDocumentContentChangeWholeDocument,
    VersionedTextDocumentIdentifier,
)

from tests.tools.lifecycle import make_client, shutdown_client
from tests.tools.compile_commands import write_cdb

POISON = (
    "int add(int a, int b) {{ return a + b; }}\n"
    "// edit {n}\n"
    "#pragma clang __debug crash\n"
)
FIXED = "int add(int a, int b) { return a + b; }\n"
HEALTHY = "int healthy(int x) { return x * 2; }\n"


def change_whole(client, uri, version, text):
    client.text_document_did_change(
        DidChangeTextDocumentParams(
            text_document=VersionedTextDocumentIdentifier(uri=uri, version=version),
            content_changes=[TextDocumentContentChangeWholeDocument(text=text)],
        )
    )


@pytest.mark.allow_anomaly
async def test_poison_quarantine(executable, tmp_path):
    (tmp_path / "healthy.cpp").write_text(HEALTHY)
    (tmp_path / "poison.cpp").write_text(POISON.format(n=0))
    write_cdb(tmp_path, ["healthy.cpp", "poison.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        healthy_uri, _ = client.open(tmp_path / "healthy.cpp")
        poison_uri, _ = client.open(tmp_path / "poison.cpp")

        hover = await client.hover_at(healthy_uri, 0, 5)
        assert hover is not None, "healthy baseline hover failed"

        async def healthy_answers(context: str):
            # The first two crashes may transiently take healthy's worker
            # with them (sub-second respawn); the guarantee under test is
            # that healthy is never PERMANENTLY lost.
            for _ in range(10):
                if await client.hover_at(healthy_uri, 0, 5) is not None:
                    return
                await asyncio.sleep(0.5)
            assert False, f"healthy hover permanently lost: {context}"

        # Hammer the poison document: two crashes reach the quarantine
        # threshold, each edit past it earns only a single probe. Healthy
        # must keep answering through all of it.
        for i in range(1, 5):
            await client.hover_at(poison_uri, 0, 5)
            change_whole(client, poison_uri, i, POISON.format(n=i))
            await asyncio.sleep(0.3)
            await healthy_answers(f"poison cycle {i}")

        # The quarantined document explains itself.
        deadline = 20
        while deadline > 0:
            diags = client.diagnostics.get(poison_uri, [])
            if any("quarantined" in d.message for d in diags):
                break
            await asyncio.sleep(0.5)
            deadline -= 1
        assert any(
            "quarantined" in d.message for d in client.diagnostics.get(poison_uri, [])
        ), f"missing quarantine diagnostic: {client.diagnostics.get(poison_uri, [])}"

        # Fixing the file earns a probe that brings it back.
        change_whole(client, poison_uri, 100, FIXED)
        recovered = None
        for _ in range(20):
            await asyncio.sleep(0.5)
            recovered = await client.hover_at(poison_uri, 0, 5)
            if recovered is not None:
                break
        assert recovered is not None, "fixed poison file did not recover"

        hover = await client.hover_at(healthy_uri, 0, 5)
        assert hover is not None, "healthy hover failed after recovery"
    finally:
        await shutdown_client(client)
