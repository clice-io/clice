import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The file sits in a subdirectory; the database is in build/ under the root.
test("a database under build/ applies below the root", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
});

test("inspect finds the root the server opens the file in", async (t) => {
    await t.cli("inspect folding-range src/main.cpp");
});
