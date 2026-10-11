import { expect } from "vitest";
import { test, type Serve, type TestContext } from "@clice/tools/testing";

const B = "export module b;\nexport int fb() { return 2; }\nint b_local() { return 3; }\n";
const IMPORTS_AB = "import a;\nimport b;\nint main() { return fa() + fb(); }\n";

/// Create a file the way an editor does: on disk, opened, saved.
async function saveNew(t: TestContext, s: Serve, file: string, text: string): Promise<void> {
    t.disk.write(file, text);
    await s.diagnostics(file);
    s.save(file);
}

test("saved module is imported", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await saveNew(t, s, "b.cppm", B);
    s.edit("main.cpp", { text: IMPORTS_AB });
    expect(await s.errors("main.cpp")).toEqual([]);
});

test("saved module survives restart", async (t) => {
    const s = await t.start();
    await saveNew(t, s, "b.cppm", B);
    // The record outlives the session.
    await s.offline(() => {
        t.disk.write("main.cpp", IMPORTS_AB);
    });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.indexed();
    await t.show("restored member", s.workspaceSymbols("b_local"));
});

test("deleted member comes back", async (t) => {
    t.disk.write("main.cpp", IMPORTS_AB);
    const s = await t.start();
    await saveNew(t, s, "b.cppm", B);
    expect(await s.errors("main.cpp")).toEqual([]);

    t.disk.rm("b.cppm");
    await s.sync({ poll: true });
    await t.show("deleted", s.errors("main.cpp"));

    // The reload rebuilds the graph from the build's units: only the kept
    // record still counts the missing file among them.
    t.disk.write("c.cpp", "int c() { return 0; }\n");
    t.disk.database({ "a.cppm": [], "main.cpp": [], "c.cpp": [] });
    expect(await s.poll("cdb")).toBe(1);

    t.disk.write("b.cppm", B);
    await s.sync({ poll: true });
    expect(await s.errors("main.cpp")).toEqual([]);
});
