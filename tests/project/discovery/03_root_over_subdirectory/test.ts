import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("root wins over subdirectory", async (t) => {
    t.disk.database({ "main.cpp": ["-DROOT"] });
    t.disk.database({ "main.cpp": ["-DSUB"], "extra.cpp": ["-DSUB"] }, "sub");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("main.cpp", s.inactiveLines("main.cpp"));
    // The subdirectory's database fills the gap.
    expect(await s.errors("extra.cpp")).toEqual([]);
});

test("the order is named", async (t) => {
    // The server logs the same hint; a batch run prints it.
    t.disk.database({ "main.cpp": ["-DROOT"] });
    t.disk.database({ "main.cpp": ["-DSUB"], "extra.cpp": ["-DSUB"] }, "sub");
    await t.cli("index --jobs 2");
});
