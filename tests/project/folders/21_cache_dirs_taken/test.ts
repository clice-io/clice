import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// a takes the client's directory and b its clice.toml's, c's default: c,
// with every directory it can use taken, serves nothing and says so.
test("every cache directory taken", async (t) => {
    const s = await t.start({ folders: ["a", "b", "c"] });
    expect(await s.errors("c/main.cpp")).toEqual([]);
    await t.show("guidance", await s.guidance());
});
