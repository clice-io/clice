import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Positions past the end of the document clamp to its end, as LSP asks,
// instead of failing the request. The file ends without a newline, on a
// line that needs formatting.

const END = { line: 3, character: "int  x = add(1,2);".length };

test(async (t) => {
    const s = await t.start();
    await s.diagnostics("main.cpp");
    const uri = s.uri("main.cpp");
    // Positions no anchor can name.
    await t.show("hover", JSON.stringify(await s.client.hoverAt(uri, 99_999, 0)));
    const past = await s.client.formatRange(uri, {
        start: { line: 0, character: 999 },
        end: { line: 9_999, character: 0 },
    });
    const clamped = await s.client.formatRange(uri, {
        start: { line: 0, character: "int add(int a, int b) {".length },
        end: END,
    });
    expect(past).toEqual(clamped);
    await t.show("formatting", s.formatting("main.cpp"));
});
