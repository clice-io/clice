import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("member hosts its headers", async (t) => {
    const s = await t.start();
    // Nothing provides Ctx yet.
    await t.show("alone", s.errors("lib/frag.h"));
    t.disk.write("lib/new.cpp", 'struct Ctx {};\n#include "frag.h"\n');
    await s.diagnostics("lib/new.cpp");
    s.save("lib/new.cpp");
    // The member lends its context to the header.
    expect(await s.errors("lib/frag.h")).toEqual([]);
});
