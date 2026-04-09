from pathlib import Path

import pytest


@pytest.mark.workspace("document_links")
async def test_document_links_with_pch(client, workspace):
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
    uri, _ = await client.open_and_wait(workspace / "main.cpp")
    links = await client.document_links(uri)

    missing_links = [
        link for link in links if Path(link.target).name == "no_such_file.bin"
    ]
    assert len(missing_links) == 0, (
        f"Expected 0 links for non-existent file, got {len(missing_links)}"
    )

    client.close(uri)
