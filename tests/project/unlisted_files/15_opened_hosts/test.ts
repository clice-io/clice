import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// No database and no rule: an opened source compiles under clice's own
// command, a member all the same, and hosts the header it includes.
test("opened source hosts its header", async (t) => {
    const s = await t.start();
    await t.show("alone", s.errors("lib/frag.h"));
    await s.diagnostics("lib/main.cpp");
    expect(await s.errors("lib/frag.h")).toEqual([]);
    await t.show("header", s.compileCommand("lib/frag.h"));
});
