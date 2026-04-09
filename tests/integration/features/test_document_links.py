"""Integration tests for document links with PCH and #embed.

Verifies that document links from both PCH (preamble) and main-file
compilation are correctly returned and merged, including #embed directives.

Test source layout (tests/data/document_links/main.cpp):

    #include "header_a.h"   <- line 0, in PCH preamble
    #include "header_b.h"   <- line 1, in PCH preamble
    int x = 1;              <- line 2, breaks preamble
    #include "header_c.h"   <- line 3, NOT in PCH (after preamble break)
                            <- line 4, empty
    const char data[] = {   <- line 5
    #embed "data.bin"       <- line 6, embed directive
    };                      <- line 7

    int main() { ... }

Expected: document_links returns 4 links total — 2 from PCH + 2 from main.
"""

from pathlib import Path

import pytest


@pytest.mark.workspace("document_links")
async def test_document_links_with_pch(client, workspace):
    """Document links from both PCH and main compilation are returned."""
    uri, content = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    assert links is not None, "document_links returned None"

    targets = sorted(Path(link.target).name for link in links)
    assert targets == ["data.bin", "header_a.h", "header_b.h", "header_c.h"], (
        f"Unexpected targets: {targets}"
    )

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_pch_portion(client, workspace):
    """PCH portion (header_a, header_b) must appear in document links."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    pch_links = [link for link in links if link.range.start.line < 2]
    assert len(pch_links) == 2, (
        f"Expected 2 PCH links (lines 0-1), got {len(pch_links)}"
    )

    pch_targets = sorted(Path(link.target).name for link in pch_links)
    assert pch_targets == ["header_a.h", "header_b.h"]

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_main_portion(client, workspace):
    """Main-file portion (header_c, data.bin) must appear in document links."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    main_links = [link for link in links if link.range.start.line >= 2]
    assert len(main_links) == 2, (
        f"Expected 2 main-file links (lines 3, 6), got {len(main_links)}"
    )

    main_targets = sorted(Path(link.target).name for link in main_links)
    assert main_targets == ["data.bin", "header_c.h"]

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_embed(client, workspace):
    """#embed directive produces a document link."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    embed_links = [link for link in links if Path(link.target).name == "data.bin"]
    assert len(embed_links) == 1, f"Expected 1 embed link, got {len(embed_links)}"
    assert embed_links[0].range.start.line == 6

    client.close(uri)
