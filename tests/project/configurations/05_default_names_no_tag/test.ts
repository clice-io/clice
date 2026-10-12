import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("misspelt default falls back", async (t) => {
    const s = await t.start();
    // The first declared tag stands in.
    await t.show("menu", s.configurations());
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("main.cpp", s.inactiveLines("main.cpp"));
    // What the user is told about the misspelt name.
    await t.show("clice.toml", s.pushes("clice.toml"));
    await t.show("guidance", await s.guidance());
});
