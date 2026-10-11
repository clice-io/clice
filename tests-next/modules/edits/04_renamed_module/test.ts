import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// a.cppm is renamed to module B and saved: the old name no longer resolves,
// the new one does.
test(async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    s.open("a.cppm");
    s.edit("a.cppm", { replace: "export module A;", with: "export module B;" });
    s.save("a.cppm");
    await t.show("old name", s.errors("main.cpp"));
    s.edit("main.cpp", { replace: "import A;", with: "import B;" });
    expect(await s.errors("main.cpp")).toEqual([]);
});
