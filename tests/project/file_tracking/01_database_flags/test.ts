import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("cdb flag change recompiles", async (t) => {
    const s = await t.start();
    await s.sync();
    await t.show("before", s.errors("main.cpp"));
    t.disk.database({ "main.cpp": ["-DFEATURE"] });
    expect(await s.poll("cdb")).toBe(1);
    // The open file picks up the new flags.
    expect(await s.errors("main.cpp")).toEqual([]);
});

test("cdb stamped tick settles", async (t) => {
    const s = await t.start();
    await s.sync();
    // An unchanged stamp is quiet.
    expect(await s.poll("cdb", { force: false })).toBe(0);
    t.disk.database({ "main.cpp": ["-DFEATURE"] });
    // A fresh stamp only arms the settling debounce; the settled one reloads.
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
});

test("cdb polling loop live", async (t) => {
    const s = await t.start({ config: { tracker: { workspace_poll_seconds: 1 } } });
    await t.show("before", s.errors("main.cpp"));
    // No poll: the database loop's own ticks see the stamp settle and
    // reload.
    const reload = await s.watch("databaseChange");
    t.disk.database({ "main.cpp": ["-DFEATURE"] });
    await reload.reached();
    expect(await s.errors("main.cpp")).toEqual([]);
});
