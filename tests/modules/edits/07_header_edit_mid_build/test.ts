import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// leaf.h, in A's global module fragment, changes on disk while A builds.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const park = await s.park("pcm", "a.cppm", { at: "reply" });
    const first = s.errors("main.cpp");
    await park.reached();
    t.disk.edit("leaf.h", { replace: "int helper()", with: "int renamed()" });
    expect(await s.poll("workspace")).toBe(1);
    await park.release();
    await first;
    await t.show("errors", s.errors("main.cpp"));
});
