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
    // alone must queue it again, and its index built under the old flags
    // answers until the reindex merges.
    const reindex = await s.park("index", "lib.cpp", { at: "reply" });
    t.disk.database({ "main.cpp": ["-DFEATURE"], "lib.cpp": ["-DFEATURE"] });
    expect(await s.poll("cdb")).toBe(1);
    await reindex.reached();
    await t.show("during", s.workspaceSymbols("feature_"));
    await reindex.release();
    await s.indexed();
    await t.show("after", s.workspaceSymbols("feature_"));
});

// The flags change while no server runs: the next one's load owes lib.cpp
// a reindex, and its index built under the old flags answers meanwhile —
// here, with indexing off, for good.
test("cdb flag change while down keeps the index", async (t) => {
    const s = await t.start();
    await s.indexed();
    await s.stop();
    t.disk.database({ "main.cpp": ["-DFEATURE"], "lib.cpp": ["-DFEATURE"] });
    const down = await t.start({ config: { project: { enable_indexing: false } } });
    await down.sync();
    await t.show("after restart", down.workspaceSymbols("feature_"));
});
