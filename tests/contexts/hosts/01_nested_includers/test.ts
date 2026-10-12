import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// utils.h and inner.h use Point, which main.cpp declares before including
// them: main.cpp is their host, inner.h's through utils.h.
test("the hosts of each file", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("utils.h");
    s.open("inner.h");
    await t.show("utils.h", s.contexts("utils.h"));
    await t.show("inner.h", s.contexts("inner.h"));
    await t.show("main.cpp", s.contexts("main.cpp"));
});

test("the automatic pick", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("utils.h");
    // Settled, so no index round moves the epoch between the two answers.
    await s.sync();
    await t.show("current", s.currentContext("utils.h"));
    const current = await s.currentContext("utils.h");
    const listed = await s.contexts("utils.h");
    expect(current.context).toEqual(listed.contexts[0]);
    expect(current.epoch).toBe(listed.epoch);
});
