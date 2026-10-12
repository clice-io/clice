import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("declared database appears", async (t) => {
    const s = await t.start();
    await t.show("before the build", s.diagnostics("main.cpp"));
    t.disk.database({ "main.cpp": ["-DFROM_BUILD"] }, "build");
    expect(await s.poll("cdb")).toBe(1);
    await t.show("after the build", s.diagnostics("main.cpp"));
});
