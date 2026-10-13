import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Read-only, the cards come from the index: the comment documenting the
// declaration in the header, not the one above the definition elsewhere.
test("index cards carry documentation", async (t) => {
    const s = await t.start({ config: { project: { readonly: "on" } } });
    s.open("main.cpp", { pull: false });
    await s.sync();
    await t.show("call", s.hover(t.marker("call")));
    await t.show("field", s.hover(t.marker("field")));
    expect((await s.counts()).pch).toBe(0);
});
