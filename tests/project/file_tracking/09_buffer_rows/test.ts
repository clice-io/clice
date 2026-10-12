import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("dependency change keeps buffer rows", async (t) => {
    t.disk.touch("h.h", new Date("2020-01-01T00:00:00Z"));
    const s = await t.start();
    await s.sync();
    expect(await s.errors("a.cpp")).toEqual([]);
    await s.indexed();
    expect(await s.errors("b.cpp")).toEqual([]);
    s.edit("b.cpp", { after: "shared_sym; }\n", insert: "// unsaved\n" });
    expect(await s.errors("b.cpp")).toEqual([]);
    const use = t.at("a.cpp", "return |shared_sym");
    await t.show("edited", s.references(use));

    // The header moves on disk: b.cpp's compile is stale, its buffer is
    // not, so the rows it compiled from these very bytes keep serving
    // while the header is indexed again.
    const reindex = await s.park("index", "h.h", { at: "reply" });
    expect(await s.poll("workspace")).toBe(0);
    t.disk.write("h.h", "#pragma once\n// moved\nextern int shared_sym;\n");
    expect(await s.poll("workspace")).toBe(1);
    await reindex.reached();
    await t.show("moved", s.references(use));
    await reindex.release();
    await s.sync();
    await t.show("reindexed", s.references(use));
});
