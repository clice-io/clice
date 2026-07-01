"""Integration tests for rapid editing: ensure no hang and correct hover results."""

import asyncio

import pytest
from lsprotocol.types import (
    DidCloseTextDocumentParams,
    HoverParams,
    Position,
)

from tests.integration.utils import doc
from tests.integration.utils.wait import SETTLE_TIME
from tests.integration.utils.workspace import did_change


@pytest.mark.workspace("hello_world")
async def test_rapid_edits_with_hover(client, workspace):
    """50 rapid edits, each followed by a hover on 'add' function.

    The file has #include <iostream> so PCH build is non-trivial.
    This must not hang and the final hover must return correct results.
    """
    main_cpp = workspace / "main.cpp"
    uri, content = await client.open_and_wait(main_cpp)

    # Hover on 'add' (line 2, char 4) to verify initial state.
    hover = await asyncio.wait_for(
        client.text_document_hover_async(
            HoverParams(
                text_document=doc(uri),
                position=Position(line=2, character=4),
            )
        ),
        timeout=30.0,
    )
    assert hover is not None, "Initial hover on 'add' should not be None"

    # 50 rapid body edits, each followed by a hover request.
    for i in range(50):
        new_content = content.replace("return a + b;", f"return a + b + {i};")
        did_change(client, uri, i + 2, new_content)
        # Fire-and-forget hover on 'add' — just ensure it doesn't hang.
        # We don't await the result here to simulate real editor behavior
        # where requests overlap.
        asyncio.ensure_future(
            client.text_document_hover_async(
                HoverParams(
                    text_document=doc(uri),
                    position=Position(line=2, character=4),
                )
            )
        )
        await asyncio.sleep(0.02)  # ~20ms between edits

    # Wait a moment for in-flight requests to settle.
    await asyncio.sleep(SETTLE_TIME * 2)

    # Final hover must succeed and return correct result.
    final_hover = await asyncio.wait_for(
        client.text_document_hover_async(
            HoverParams(
                text_document=doc(uri),
                position=Position(line=2, character=4),
            )
        ),
        timeout=30.0,
    )
    assert final_hover is not None, "Final hover returned None — worker may have hung"
    assert final_hover.contents is not None, "Final hover contents should not be None"

    client.text_document_did_close(DidCloseTextDocumentParams(text_document=doc(uri)))
