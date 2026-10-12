import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("menu", s.configurations());
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("main.cpp", s.inactiveLines("main.cpp"));
});
