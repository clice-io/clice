import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("build that climbs out of its directory", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    await t.show("links", s.documentLinks("src/main.cpp"));
    await t.show("lib_fn", s.definition(t.at("src/main.cpp", "return |lib_fn")));
    await s.indexed();
    await t.show("common_fn", s.workspaceSymbols("common_fn"));
});
