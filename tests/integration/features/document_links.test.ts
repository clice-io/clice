import { cliceTest, expect } from "../../fixtures.ts";

const test = cliceTest("document_links");

function targetName(target: string): string {
    return target.split("/").pop() ?? target;
}

test("document links with pch", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    expect(links, "document_links returned None").not.toBeNull();

    const targets = links!.map((link) => targetName(link.target!)).sort();
    expect(targets, `Unexpected targets: ${targets.join(", ")}`).toEqual([
        "data.bin",
        "data.bin",
        "header_a.h",
        "header_b.h",
        "header_c.h",
    ]);

    client.close(uri);
});

test("document links pch portion", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    const pchLinks = links!.filter((link) => link.range.start.line < 2);
    expect(pchLinks.length, `Expected 2 PCH links (lines 0-1), got ${pchLinks.length}`).toBe(2);

    const pchTargets = pchLinks.map((link) => targetName(link.target!)).sort();
    expect(pchTargets).toEqual(["header_a.h", "header_b.h"]);

    client.close(uri);
});

test("document links main portion", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    const mainLinks = links!.filter((link) => link.range.start.line >= 2);
    expect(
        mainLinks.length,
        `Expected 3 main-file links (lines 3, 6, 9), got ${mainLinks.length}`,
    ).toBe(3);

    const mainTargets = mainLinks.map((link) => targetName(link.target!)).sort();
    expect(mainTargets).toEqual(["data.bin", "data.bin", "header_c.h"]);

    client.close(uri);
});

test("document links embed", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    const embedLinks = links!.filter(
        (link) => targetName(link.target!) === "data.bin" && link.range.start.line === 6,
    );
    expect(embedLinks.length, `Expected 1 embed link at line 6, got ${embedLinks.length}`).toBe(1);

    client.close(uri);
});

test("document links has embed exists", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    const hasEmbedLinks = links!.filter(
        (link) => targetName(link.target!) === "data.bin" && link.range.start.line === 9,
    );
    expect(
        hasEmbedLinks.length,
        `Expected 1 has_embed link at line 9, got ${hasEmbedLinks.length}`,
    ).toBe(1);

    client.close(uri);
});

test("document links has embed missing", async ({ client }) => {
    const [uri] = await client.openAndWait("main.cpp");
    const links = await client.documentLinks(uri);

    const missingLinks = links!.filter((link) => targetName(link.target!) === "no_such_file.bin");
    expect(
        missingLinks.length,
        `Expected 0 links for non-existent file, got ${missingLinks.length}`,
    ).toBe(0);

    client.close(uri);
});
