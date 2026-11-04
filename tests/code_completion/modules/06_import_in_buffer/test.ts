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

// A module imported below an include: the index its PCM build wrote offers
// its names, beside the preamble's.
test("an import below an include", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("math.cppm")).toEqual([]);
    s.open("main.cpp");
    s.edit("main.cpp", {
        text: '#include "counter.h"\nimport math;\nint total = add(1, 2) + count();\n',
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    s.edit("main.cpp", { replace: "add(1, 2)", with: "ad" });
    await t.show("completion", s.completion(t.at("main.cpp", "= ad| +")));
});
