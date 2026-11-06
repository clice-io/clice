import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// objects.c expands one table twice, as NetHack's src/objects.c does: its
// own unit is the first pass, the copy of itself it includes at its end the
// second. The index merges both passes; the AST compiles one.

test("closed", async (t) => {
    const s = await t.start();
    await s.indexed();
    await t.show("main.c", {
        objects: s.definition(t.marker("use")),
        descr: s.definition(t.marker("use_descr")),
        symbols: s.workspaceSymbols("obj"),
    });
});

test("open", async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(await s.errors("objects.c")).toEqual([]);
    await t.show("objects.c", s.contexts("objects.c"));
    await t.show("main.c", {
        objects: s.definition(t.marker("use")),
        symbols: s.workspaceSymbols("obj"),
    });
});
