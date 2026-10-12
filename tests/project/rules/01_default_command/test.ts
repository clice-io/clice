import { expect } from "vitest";
import { test } from "@clice/tools/testing";

const GATED = "#ifndef FEATURE\n#error missing FEATURE\n#endif\nint later() { return 0; }\n";

test("default command compiles files", async (t) => {
    const s = await t.start();
    // No error, and no note that the command was guessed: it was declared.
    await t.show("src/main.cpp", s.diagnostics("src/main.cpp"));
    // A file created after startup takes the same command at once.
    t.disk.write("src/later.cpp", GATED);
    await t.show("src/later.cpp", s.diagnostics("src/later.cpp"));
});

test("new files join the index", async (t) => {
    const s = await t.start();
    expect(await s.errors("src/main.cpp")).toEqual([]);
    expect(await s.poll("workspace")).toBe(0);
    t.disk.write("src/later.cpp", "int later_entry() { return 1; }\n");
    expect(await s.poll("workspace")).toBe(1);
    await s.indexed();
    await t.show("symbols", s.workspaceSymbols("later_entry"));
});
