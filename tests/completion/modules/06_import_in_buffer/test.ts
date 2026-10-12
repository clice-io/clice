import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// An import typed into the buffer and not saved is scanned from the buffer:
// the module it names is built, and its names complete.
test("an unsaved import", async (t) => {
    const s = await t.start();
    expect(await s.errors("math.cppm")).toEqual([]);
    s.open("main.cpp");
    s.edit("main.cpp", { text: "import math;\nint total = add(1, 2);\n" });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "add(1, 2)", with: "ad" });
    await t.show("completion", s.completion(t.at("main.cpp", "= ad|;")));
});
