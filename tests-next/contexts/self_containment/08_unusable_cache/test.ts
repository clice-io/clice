import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The includer's context lives in memory: a cache directory the server
// cannot use leaves it whole.
test(async (t) => {
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("utils.h", s.diagnostics("utils.h"));
});
