import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("cdb appears after startup", async (t) => {
    const s = await t.start();
    await s.sync();
    await t.show("guessed", s.errors("main.cpp"));
    t.disk.database({ "main.cpp": ["-DFEATURE"], "lib.cpp": ["-DFEATURE"] });
    expect(await s.poll("cdb")).toBe(1);
    // The open file switches to the found database, and its closed units
    // are indexed.
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("indexed", s.workspaceSymbols("lib_entry"));
});
