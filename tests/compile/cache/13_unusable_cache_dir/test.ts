import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The cache directory cannot be created (a file sits where it goes): the
// project serves nothing, and the server tells the user which directory and
// what to do.
test("server", { errors: ["Cannot create the log directory"] }, async (t) => {
    t.disk.write(".clice", "not a directory\n");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("guidance", await s.guidance());
});

// A lint run builds its preambles into the cache too: it stops at once.
test("lint", { errors: ["Cannot create the log directory", "Cannot use the cache directory"] }, async (t) => {
    t.disk.write(".clice", "not a directory\n");
    await t.cli("lint");
});
