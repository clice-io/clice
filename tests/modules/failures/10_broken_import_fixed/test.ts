import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails because B does; a tool fixes B on disk, nothing is saved, and
// main.cpp asks before any tick.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("b.cppm", { replace: "return broken_in_b;", with: "return 1;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
