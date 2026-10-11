import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Every case marks the declaration main.cpp uses: an edit made to it while
// the server was down reaches main.cpp, and its precompiled parts outlive a
// restart.
test("cache survives a restart", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    await s.restart();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    const after = await s.counts();
    expect(after.pch).toBe(0);
    expect(after.pcm).toBe(0);
});

test("offline edit reaches its importers", async (t) => {
    const s = await t.start();
    expect(await s.diagnostics("main.cpp")).toEqual([]);
    const declaration = t.marker("declaration");
    await s.offline(() => {
        t.disk.edit(declaration.path, { replace: "double area(", with: "double surface(" });
    });
    expect(await s.errors("main.cpp")).not.toEqual([]);
});
