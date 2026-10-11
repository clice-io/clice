import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

async function pinSecond(s: Serve): Promise<void> {
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("list.def");
    expect((await s.switchContext("list.def", "main.cpp", { occurrence: 1 })).success).toBe(true);
    expect(await s.errors("list.def")).toEqual([]);
}

// Closed and opened again, the header keeps its include and the preamble
// built for it.
test("a reopen keeps the choice", async (t) => {
    const s = await t.start();
    await pinSecond(s);
    const built = (await s.counts()).pch;
    s.close("list.def");
    s.open("list.def");
    await t.show("current", s.currentContext("list.def"));
    await t.show("diagnostics", s.diagnostics("list.def"));
    expect((await s.counts()).pch).toBe(built);
});

// The includer changes on disk while the header is closed: what the context
// holds of it is stale, and the reopen reads it again.
test("a changed includer is read again", async (t) => {
    const s = await t.start();
    await pinSecond(s);
    s.close("list.def");
    const written = t.disk.mtime("main.cpp");
    t.disk.edit("main.cpp", {
        replace: "void get_##name();",
        with: "int get_##name = missing_value;",
    });
    t.disk.touch("main.cpp", new Date(written.getTime() + 10_000));
    s.open("list.def");
    await t.show("current", s.currentContext("list.def"));
    await t.show("diagnostics", s.diagnostics("list.def"));
});
