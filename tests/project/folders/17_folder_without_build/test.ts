import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

/// Whether the directory holds any file.
function exists(t: TestContext, dir: string): boolean {
    return t.disk.list(`${dir}/**`).length > 0;
}

test("a folder with no build keeps nothing on disk", async (t) => {
    const s = await t.start({ folders: ["alpha", "plain"] });
    await s.indexed();
    await s.stop();
    expect(exists(t, "alpha/.clice")).toBe(false);
    expect(exists(t, "plain/.clice")).toBe(false);
    expect(exists(t, ".clice/logs")).toBe(true);
});

test("opening a file of it opens it", async (t) => {
    const s = await t.start({ folders: ["alpha", "plain"] });
    expect(await s.errors("plain/main.cpp")).toEqual([]);
    await s.stop();
    expect(exists(t, "plain/.clice")).toBe(true);
});

test("the log goes with the first project opened", async (t) => {
    // plain takes the client's cache directory, which stays unused; alpha
    // falls back to its own, and the session logs there.
    const s = await t.start({ folders: ["plain", "alpha"] });
    await s.indexed();
    await s.stop();
    expect(exists(t, ".clice")).toBe(false);
    expect(t.disk.list("alpha/.clice/logs/*/master.log")).toHaveLength(1);
});

// A database the waiting folder finds later takes the open files it lists
// from the folder that held them.
test("a database found later takes its files", async (t) => {
    const s = await t.start({ folders: ["alpha", "plain"] });
    await t.show("before", s.errors("alpha/extra.cpp"));
    t.disk.database({ "alpha/extra.cpp": ["-DFROM_PLAIN"] }, "plain");
    expect(await s.poll("cdb")).toBe(1);
    await t.show("after", s.errors("alpha/extra.cpp"));
});
