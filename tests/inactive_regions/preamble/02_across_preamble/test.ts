import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The header opens in the context of render_vk.cpp, its only host. The #if
// sits among its leading directives; the #elif that ends the taken branch
// lies past them.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("render.h")).toEqual([]);
    await t.show("render.h", s.inactiveLines("render.h"));
});
