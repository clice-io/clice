import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("rule binds a subtree to its database", async (t) => {
    t.disk.database({ "src/a.cpp": ["-DROOT"], "lib/x.cpp": ["-DROOT"] }, "cmake");
    t.disk.database({ "lib/x.cpp": ["-DLIB"] }, "lib/cmake");
    const s = await t.start();
    // src/ keeps the workspace's database; lib/ takes its rule's first.
    expect(await s.errors("src/a.cpp")).toEqual([]);
    expect(await s.errors("lib/x.cpp")).toEqual([]);
});
