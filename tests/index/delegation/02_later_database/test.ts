import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The server started before the project had a database, and holds nothing
// of it: once it finds the database, it opens the project and indexes for
// `clice index`.
test("none at first", async (t) => {
    const s = await t.start();
    await s.sync();
    t.disk.database({ "main.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    await t.cli("index");
    await s.sync();
    await t.show("symbols", s.workspaceSymbols("compute"));
});

// A database listing no unit opens the project: the server answers there
// is nothing to index, then indexes once the database lists one.
test("empty at first", async (t) => {
    t.disk.database({});
    const s = await t.start();
    await s.sync();
    await t.cli("index");
    t.disk.database({ "main.cpp": [] });
    await t.cli("index");
    await s.sync();
    await t.show("symbols", s.workspaceSymbols("compute"));
});
