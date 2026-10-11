import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

function original(t: TestContext): void {
    t.disk.database({ "main.cpp": ["-DFEATURE"], "only.cpp": ["-DFEATURE"] }, "build");
}

test("vanished database yields to present", async (t) => {
    original(t);
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("build", s.inactiveLines("main.cpp"));
    expect(await s.errors("only.cpp")).toEqual([]);

    // The build directory is wiped: alone, a vanished database stays silent.
    t.disk.rm("build/compile_commands.json");
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(0);

    // Regenerated elsewhere: the files both databases list follow the
    // present one, the rest keep serving.
    t.disk.database({ "main.cpp": ["-DFEATURE", "-DMOVED"] }, "out");
    // The new database settles for a tick.
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("out", {
        main: s.inactiveLines("main.cpp"),
        "main contexts": s.contexts("main.cpp"),
        "only contexts": s.contexts("only.cpp"),
    });

    // The returning database takes its place back.
    original(t);
    expect(await s.poll("cdb", { force: false })).toBe(0);
    expect(await s.poll("cdb", { force: false })).toBe(1);
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("back", s.inactiveLines("main.cpp"));
});
