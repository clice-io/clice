import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("saved partition joins module", async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.write("a-part.cppm", "export module a:part;\nexport int fpart() { return 3; }\n");
    await s.diagnostics("a-part.cppm");
    s.save("a-part.cppm");
    expect(await s.errors("main.cpp")).toEqual([]);
});
