import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Three modules import each other in a ring: none builds, each names the
// whole ring, and the rest of the project is unaffected.
test(async (t) => {
    const s = await t.start();
    await t.show("ring_a.cppm", s.errors("ring_a.cppm"));
    await t.show("ring_b.cppm", s.errors("ring_b.cppm"));
    await t.show("ring_c.cppm", s.errors("ring_c.cppm"));
    expect(await s.errors("ok.cppm")).toEqual([]);
});
