import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A fails for what B lacks; once B changes, A builds again.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.errors("main.cpp"));
    t.disk.edit("b.cppm", { replace: "export int b()", with: "export int c()" });
    s.save("b.cppm");
    expect(await s.errors("main.cpp")).toEqual([]);
});
