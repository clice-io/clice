import * as path from "node:path";

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

test("a file outside every folder opens neither", async (t) => {
    t.disk.write("stray.h", "#pragma once\nint stray();\n");
    const s = await t.start({ folders: ["plain", "alpha"] });
    expect(await s.errors("stray.h")).toEqual([]);
    await s.stop();
    expect(exists(t, "plain/.clice")).toBe(false);
    expect(exists(t, ".clice/cache")).toBe(false);
});

// A rule the editor sends applies to every folder: its default command
// makes no folder a build of its own.
test("the editor's default command opens no folder", async (t) => {
    const s = await t.start({
        folders: ["alpha", "plain"],
        config: { rules: [{ default_command: "clang++ -std=c++20" }] },
    });
    await s.indexed();
    await s.stop();
    expect(exists(t, "plain/.clice")).toBe(false);
});

// A rule of the user's file applies to every folder too.
test("the user's default command opens no folder", async (t) => {
    t.disk.write("user/clice/clice.toml", '[[rules]]\ndefault_command = "clang++ -std=c++20"\n');
    const s = await t.start({
        folders: ["alpha", "plain"],
        env: { XDG_CONFIG_HOME: path.join(t.root, "user") },
    });
    await s.indexed();
    await s.stop();
    expect(exists(t, "plain/.clice")).toBe(false);
});
