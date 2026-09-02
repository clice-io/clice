/// Tests for the generated-region helpers shared by the docs generators
/// (tools/docs/generated.ts).

import { expect, test } from "vitest";
import { renderMarkdownTable, rewriteRegions } from "@clice/tools/docs/generated";

const MARKERS = { begin: /^<!-- BEGIN X: (.+?) -->$/, end: "<!-- END X -->" };

test("regions are rewritten in place, the page around them kept", () => {
    const page = ["# Title", "", "<!-- BEGIN X: a -->", "old", "<!-- END X -->", "", "tail"].join(
        "\n",
    );
    const problems: string[] = [];
    const { text, seen } = rewriteRegions(page, "p.md", MARKERS, (key) => `new ${key}`, problems);
    expect(text).toBe(
        ["# Title", "", "<!-- BEGIN X: a -->", "", "new a", "", "<!-- END X -->", "", "tail"].join(
            "\n",
        ),
    );
    expect([...seen]).toEqual(["a"]);
    expect(problems).toEqual([]);
    // An empty body leaves one blank line between the markers.
    expect(rewriteRegions(page, "p.md", MARKERS, () => "", problems).text).toContain(
        "<!-- BEGIN X: a -->\n\n<!-- END X -->",
    );
});

test("duplicate and unclosed regions are reported, not mangled", () => {
    const problems: string[] = [];
    const page = [
        "<!-- BEGIN X: a -->",
        "<!-- END X -->",
        "<!-- BEGIN X: a -->",
        "<!-- END X -->",
        "<!-- BEGIN X: b -->",
        "never closed",
    ].join("\n");
    const { text } = rewriteRegions(page, "p.md", MARKERS, (key) => key, problems);
    expect(problems).toEqual([
        "p.md: duplicate region 'a'",
        "p.md: region 'b' has no closing marker",
    ]);
    // The unclosed region and everything after it stay as written.
    expect(text.endsWith("<!-- BEGIN X: b -->\nnever closed")).toBe(true);
});

test("tables pad columns the way prettier does", () => {
    expect(
        renderMarkdownTable([
            ["A", "Long"],
            ["xx", "y"],
        ]),
    ).toEqual(["| A  | Long |", "| -- | ---- |", "| xx | y    |"]);
});
