import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("lost lender drops member", async (t) => {
    const s = await t.start();
    s.open("src/a.cpp");
    t.disk.write("tools/tool.c", "int tool_fn(void) { return 1; }\n");
    await s.diagnostics("tools/tool.c");
    s.save("tools/tool.c");
    s.close("tools/tool.c");
    await s.indexed();
    await t.show("saved", s.workspaceSymbols("tool_fn"));

    // A .c with no C unit left to borrow from leaves the index.
    t.disk.database({ "src/a.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);
    await s.indexed();
    await t.show("no lender", s.workspaceSymbols("tool_fn"));
});
