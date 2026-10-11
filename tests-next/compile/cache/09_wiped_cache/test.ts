import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("wiped while running", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    // The user resets the cache without restarting the server; the next
    // PCH build creates the store's directories again.
    t.disk.rm(".clice/cache");
    t.disk.write("header.h", "#pragma once\n\nstruct W {\n    int x;\n    int y;\n};\n");
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(2);
    expect(t.disk.list(".clice/cache/*/pch/*.pch")).toHaveLength(1);
});
