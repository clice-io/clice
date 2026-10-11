import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("created header reaches includers", async (t) => {
    // Where a failed include looked is watched: creating the header there
    // compiles the open includer again and indexes the closed one again.
    const s = await t.start();
    await s.sync();
    await t.show("missing", s.errors("open.cpp"));
    await s.indexed();
    t.disk.write("gen.h", "int make();\n");
    expect(await s.poll("workspace")).toBe(1);
    expect(await s.errors("open.cpp")).toEqual([]);
    const { failed } = await s.sync();
    await t.show("failed", failed);
    await t.show("created", s.references(t.at("gen.h", "int |make")));
});
