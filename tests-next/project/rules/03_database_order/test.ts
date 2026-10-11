import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test(async (t) => {
    t.disk.database({ "main.cpp": ["-DFROM_A"] }, "a");
    t.disk.database({ "main.cpp": ["-DFROM_B"], "other.cpp": ["-DFROM_B"] }, "b");
    const s = await t.start();
    // The first database wins for a file both list; the second fills in.
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await s.errors("other.cpp")).toEqual([]);
    await t.show("main.cpp", s.contexts("main.cpp"));
});
