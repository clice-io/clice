import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails because B does; a tool drops A's import of B on disk, nothing is
// saved, and main.cpp is edited before any tick: A builds again, though B
// still fails.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("a.cppm", { replace: "import B;\n", with: "" });
    t.disk.edit("a.cppm", { replace: "return b();", with: "return 1;" });
    s.edit("main.cpp", { replace: "return a();", with: "return a() + 0;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
