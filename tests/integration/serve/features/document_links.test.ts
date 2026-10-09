/// Behavioral document-links tests for the PCH/main split: the snap suite
/// pins the merged reply, these pin that both halves contribute — links
/// from the preamble (compiled into the PCH) and from the main portion
/// must all survive the reply-edge merge.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

function targetName(target: string): string {
    return target.split("/").pop() ?? target;
}

/// `int x = 1;` on line 2 ends the preamble: the two includes above it
/// compile into the PCH, everything below stays in the main portion.
const test = serve.files({
    "header_a.h": "#pragma once\n\nint a = 1;\n",
    "header_b.h": "#pragma once\n\nint b = 1;\n",
    "header_c.h": "#pragma once\n\nint c = 1;\n",
    "data.bin": "clice",
    "main.cpp":
        '#include "header_a.h"\n' +
        '#include "header_b.h"\n' +
        "int x = 1;\n" +
        '#include "header_c.h"\n' +
        "\n" +
        "const char data[] = {\n" +
        '#embed "data.bin"\n' +
        "};\n" +
        "\n" +
        '#if __has_embed("data.bin")\n' +
        "int has_embed_found = 1;\n" +
        "#endif\n" +
        "\n" +
        '#if __has_embed("no_such_file.bin")\n' +
        "int has_embed_not_found = 1;\n" +
        "#endif\n" +
        "\n" +
        "int main() {\n" +
        "    return a + b + c + x;\n" +
        "}\n",
});

async function openMain(s: Serve): Promise<proto.DocumentLink[]> {
    await s.compiled("main.cpp");
    const links = (await s.request("textDocument/documentLink", "main.cpp")) as
        proto.DocumentLink[] | null;
    expect(links, "document_links returned None").not.toBeNull();
    return links ?? [];
}

test("document links with pch", async ({ s }) => {
    const links = await openMain(s);
    const targets = links.map((link) => targetName(link.target ?? "")).sort();
    expect(targets, `Unexpected targets: ${targets.join(", ")}`).toEqual([
        "data.bin",
        "data.bin",
        "header_a.h",
        "header_b.h",
        "header_c.h",
    ]);
});

test("document links pch portion", async ({ s }) => {
    const links = await openMain(s);
    const pchLinks = links.filter((link) => link.range.start.line < 2);
    expect(pchLinks.length, `Expected 2 PCH links (lines 0-1), got ${pchLinks.length}`).toBe(2);
    const pchTargets = pchLinks.map((link) => targetName(link.target ?? "")).sort();
    expect(pchTargets).toEqual(["header_a.h", "header_b.h"]);
});

test("document links main portion", async ({ s }) => {
    const links = await openMain(s);
    const mainLinks = links.filter((link) => link.range.start.line >= 2);
    expect(
        mainLinks.length,
        `Expected 3 main-file links (lines 3, 6, 9), got ${mainLinks.length}`,
    ).toBe(3);
    const mainTargets = mainLinks.map((link) => targetName(link.target ?? "")).sort();
    expect(mainTargets).toEqual(["data.bin", "data.bin", "header_c.h"]);
});

test("document links embed", async ({ s }) => {
    const links = await openMain(s);
    const embedLinks = links.filter(
        (link) => targetName(link.target ?? "") === "data.bin" && link.range.start.line === 6,
    );
    expect(embedLinks.length, `Expected 1 embed link at line 6, got ${embedLinks.length}`).toBe(1);
});

test("document links has embed exists", async ({ s }) => {
    const links = await openMain(s);
    const hasEmbedLinks = links.filter(
        (link) => targetName(link.target ?? "") === "data.bin" && link.range.start.line === 9,
    );
    expect(
        hasEmbedLinks.length,
        `Expected 1 has_embed link at line 9, got ${hasEmbedLinks.length}`,
    ).toBe(1);
});

test("document links has embed missing", async ({ s }) => {
    const links = await openMain(s);
    const missingLinks = links.filter(
        (link) => targetName(link.target ?? "") === "no_such_file.bin",
    );
    expect(
        missingLinks.length,
        `Expected 0 links for non-existent file, got ${missingLinks.length}`,
    ).toBe(0);
});
