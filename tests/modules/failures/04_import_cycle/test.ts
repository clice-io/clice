import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// Two modules import each other: neither builds, a module importing them
// through another reports the cycle too, and the server stays responsive
// for the rest of the project.
test(async (t) => {
    const s = await t.start();
    await t.show("cycle_a.cppm", s.errors("cycle_a.cppm"));
    await t.show("cycle_b.cppm", s.errors("cycle_b.cppm"));
    await t.show("entry.cppm", s.errors("entry.cppm"));
    expect(await s.errors("ok.cppm")).toEqual([]);
});
