"""Client $/cancelRequest reaches the worker (end-to-end cancellation)."""

import asyncio
import uuid

import pytest
from lsprotocol.types import (
    CancelParams,
    CompletionParams,
    Position,
    TextDocumentIdentifier,
)

from tests.tools.compile_commands import write_cdb
from tests.tools.lifecycle import make_client, shutdown_client

# Two hundred thousand trivial declarations: slow to parse on any hardware,
# cheap to abandon (the worker polls the stop flag per declaration).
SLOW = "\n".join(f"int v{i};" for i in range(200_000)) + "\n"


async def test_cancelled_completion_replies(executable, tmp_path):
    (tmp_path / "slow.cpp").write_text(SLOW)
    write_cdb(tmp_path, ["slow.cpp"])

    client = await make_client(executable, tmp_path)
    try:
        uri, _ = client.open(tmp_path / "slow.cpp")

        # Complete at the LAST line: clang truncates the parse at the
        # completion point, so a point at the top would skip the slow body
        # entirely and the request could finish before the cancel arrives.
        params = CompletionParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=199_999, character=4),
        )
        msg_id = str(uuid.uuid4())
        task = asyncio.ensure_future(
            client.protocol.send_request_async(
                "textDocument/completion", params, msg_id=msg_id
            )
        )
        await asyncio.sleep(0.1)
        client.protocol.notify("$/cancelRequest", CancelParams(id=msg_id))

        # The cancel must produce a prompt error reply, not a full compile.
        with pytest.raises(Exception):
            await asyncio.wait_for(task, timeout=30)

        # The server survives the cancellation and still answers.
        hover = await client.hover_at(uri, 0, 5)
        assert hover is not None
    finally:
        await shutdown_client(client)
