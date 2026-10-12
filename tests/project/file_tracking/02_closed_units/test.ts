import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("cdb new entry indexed", async (t) => {
    t.disk.database({ "main.cpp": [] });
    const s = await t.start();
    await s.sync();
    expect(await s.errors("main.cpp")).toEqual([]);
    t.disk.database({ "main.cpp": [], "lib.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    await s.indexed();
    await t.show("added", s.workspaceSymbols("feature_"));
});

test("cdb flag change reindexes closed", async (t) => {
    const s = await t.start();
    await s.sync();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("before", s.workspaceSymbols("feature_"));
    // Only lib.cpp's flags change, not its bytes: the database's change
    // alone must queue it again.
    t.disk.database({ "main.cpp": ["-DFEATURE"], "lib.cpp": ["-DFEATURE"] });
    expect(await s.poll("cdb")).toBe(1);
    await s.indexed();
    await t.show("after", s.workspaceSymbols("feature_"));
});
