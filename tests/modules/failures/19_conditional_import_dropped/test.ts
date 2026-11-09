import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A imports B, which fails, as long as its config.h says so; a tool turns
// the import off in config.h on disk, nothing is saved, and main.cpp is
// edited before any tick: A builds again, though B still fails.
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("config.h", { replace: "USE_B 1", with: "USE_B 0" });
    s.edit("main.cpp", { replace: "return a();", with: "return a() + 0;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
