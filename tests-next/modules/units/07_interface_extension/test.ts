import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    expect(await s.errors("a.ixx")).toEqual([]);
    await t.show("main.cpp", s.errors("main.cpp"));
    await t.show("use", s.hover(t.at("main.cpp", "return |a()")));
});
