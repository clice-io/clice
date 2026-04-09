"""Integration tests for document links with PCH, #embed, and __has_embed.

Verifies that document links from both PCH (preamble) and main-file
compilation are correctly returned and merged, including #embed and
__has_embed directives.

Test source layout (tests/data/document_links/main.cpp):

    #include "header_a.h"              <- line 0, in PCH preamble
    #include "header_b.h"              <- line 1, in PCH preamble
    int x = 1;                         <- line 2, breaks preamble
    #include "header_c.h"              <- line 3
                                       <- line 4, empty
    const char data[] = {              <- line 5
    #embed "data.bin"                  <- line 6, embed directive
    };                                 <- line 7
                                       <- line 8, empty
    #if __has_embed("data.bin")        <- line 9, has_embed (file exists)
    int has_embed_found = 1;           <- line 10
    #endif                             <- line 11
                                       <- line 12, empty
    #if __has_embed("no_such_file.bin") <- line 13, has_embed (file missing)
    int has_embed_not_found = 1;       <- line 14
    #endif                             <- line 15

Expected: 5 links from includes + embed + has_embed(existing),
          __has_embed("no_such_file.bin") produces NO link.
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
    assert targets == [
        "data.bin",
        "data.bin",
        "header_a.h",
        "header_b.h",
        "header_c.h",
    ], f"Unexpected targets: {targets}"

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
    """Main-file portion (header_c, data.bin x2) must appear in document links."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    main_links = [link for link in links if link.range.start.line >= 2]
    assert len(main_links) == 3, (
        f"Expected 3 main-file links (lines 3, 6, 9), got {len(main_links)}"
    )

    main_targets = sorted(Path(link.target).name for link in main_links)
    assert main_targets == ["data.bin", "data.bin", "header_c.h"]

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_embed(client, workspace):
    """#embed directive produces a document link."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    embed_links = [
        link
        for link in links
        if Path(link.target).name == "data.bin" and link.range.start.line == 6
    ]
    assert len(embed_links) == 1, (
        f"Expected 1 embed link at line 6, got {len(embed_links)}"
    )

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_has_embed_exists(client, workspace):
    """__has_embed with an existing file produces a document link."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    has_embed_links = [
        link
        for link in links
        if Path(link.target).name == "data.bin" and link.range.start.line == 9
    ]
    assert len(has_embed_links) == 1, (
        f"Expected 1 has_embed link at line 9, got {len(has_embed_links)}"
    )

    client.close(uri)


@pytest.mark.workspace("document_links")
async def test_document_links_has_embed_missing(client, workspace):
    """__has_embed with a non-existent file produces NO document link."""
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    missing_links = [
        link for link in links if Path(link.target).name == "no_such_file.bin"
    ]
    assert len(missing_links) == 0, (
        f"Expected 0 links for non-existent file, got {len(missing_links)}"
    )

    client.close(uri)
