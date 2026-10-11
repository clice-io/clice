import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("macro include change reindexes", async (t) => {
    t.disk.touch("header.h", new Date("2020-01-01T00:00:00Z"));
    const s = await t.start();
    await s.indexed();
    await t.show("v1", s.references(t.at("header.h", "inline int |alpha")));
    expect(await s.poll("workspace")).toBe(0);
    // Only the compile resolves the include: the header is watched, and its
    // includer found, through what the indexed compile read.
    t.disk.edit("header.h", { replace: "TARGET alpha", with: "TARGET beta" });
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    await t.show("v2", s.references(t.at("header.h", "inline int |beta")));
});
