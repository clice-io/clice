import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// mode.h compiles on its own, without FAST. A host the user picks is picked
// for what it defines first: the header then reads FAST.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("a.cpp")).toEqual([]);
    expect(await s.errors("mode.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(0);
    await t.show("alone", s.inactiveLines("mode.h"));
    expect((await s.switchContext("mode.h", "a.cpp")).success).toBe(true);
    await s.sync();
    expect(await s.errors("mode.h")).toEqual([]);
    expect((await s.stats()).synthesizedContexts).toBe(1);
    await t.show("in a.cpp", s.inactiveLines("mode.h"));
});
