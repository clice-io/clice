"""Wait and polling helpers for integration tests."""

import asyncio

from lsprotocol.types import (
    HoverParams,
    Position,
    TextDocumentIdentifier,
    WorkspaceSymbolParams,
)

# Standard timing constants — use these instead of hardcoded sleep values.
MTIME_GRANULARITY = 1.1  # Filesystem mtime precision (1s on many FSes, +0.1 margin)
SETTLE_TIME = 0.5  # Time for server to stabilize after an operation
IDLE_TIMEOUT = 5.0  # Time to wait for server idle in lifecycle tests


async def wait_for_recompile(client, uri: str, *, timeout: float = 60.0) -> None:
    """Trigger recompilation via hover and wait for fresh diagnostics.

    Useful after didChange or on-disk file modifications. Sends a hover
    request at (0,0) to trigger ensure_compiled(), then waits for the
    resulting diagnostics notification.
    """
    event = client.wait_for_diagnostics(uri)
    await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )
    await asyncio.wait_for(event.wait(), timeout=timeout)


async def wait_for_index(
    client,
    uri: str,
    symbol_name: str = "add",
    *,
    timeout: int = 30,
) -> bool:
    """Poll workspace/symbol until a specific symbol appears in the index.

    Sends a hover to trigger compilation/indexing, then polls every second.
    Returns True if the symbol was found, False on timeout.
    """
    await client.text_document_hover_async(
        HoverParams(
            text_document=TextDocumentIdentifier(uri=uri),
            position=Position(line=0, character=0),
        )
    )

    for _ in range(timeout):
        result = await client.workspace_symbol_async(
            WorkspaceSymbolParams(query=symbol_name)
        )
        if result and any(s.name == symbol_name for s in result):
            return True
        await asyncio.sleep(1)
    return False
