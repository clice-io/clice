import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// clice/internal/sync answers once the server has no work left: the
// background index included, and after a look at the disk when asked to;
// short of its deadline it names the work still pending.

const SETTLED = { failed: [], unsaved: false, pending: [] };

test("waits for the index", async (t) => {
    const s = await t.start();
    // The hook's raw reply: the pending list is what it is about.
    expect(await s.client.sync()).toEqual(SETTLED);
    await t.show("symbols", s.workspaceSymbols("fn_"));
    const { files } = await s.counts();
    await t.show("index builds", {
        "a.cpp": files["a.cpp"]?.index ?? 0,
        "b.cpp": files["b.cpp"]?.index ?? 0,
        "c.cpp": files["c.cpp"]?.index ?? 0,
    });
});

test("looks at the disk first", async (t) => {
    const s = await t.start();
    await s.sync();
    // A rewrite of another size: the look sees it at once.
    t.disk.write("a.cpp", "int fn_renamed() { return 1; }\n");
    expect(await s.client.sync({ poll: true })).toEqual(SETTLED);
    await t.show("symbols", s.workspaceSymbols("fn_renamed"));
});

test("names held work", async (t) => {
    const s = await t.start();
    await s.sync();
    // The hold is in place before the database names d.cpp.
    t.disk.write("d.cpp", "int fn_d() { return 2; }\n");
    t.disk.database();
    const park = await s.park("index", "d.cpp", { at: "reply" });
    await s.poll("cdb");
    await park.reached();
    const held = await s.client.sync({ deadlineMs: 1_000 });
    await t.show("pending", held.pending);
    await park.release();
    expect(await s.client.sync()).toEqual(SETTLED);
    await t.show("symbols", s.workspaceSymbols("fn_d"));
});
