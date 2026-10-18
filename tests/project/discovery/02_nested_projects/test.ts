import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("nested projects load on open", async (t) => {
    const s = await t.start();
    // Opening a file registers the database of its project.
    expect(await s.errors("group-a/p1/main.cpp")).toEqual([]);
    expect(await s.errors("group-a/p2/main.cpp")).toEqual([]);
    // Both list the generated file: p1's command, loaded first, is its default.
    expect(await s.errors("group-a/shared/generated.cpp")).toEqual([]);
    await t.show("generated", {
        inactive: s.inactiveLines("group-a/shared/generated.cpp"),
        contexts: s.contexts("group-a/shared/generated.cpp"),
    });
    await s.indexed();
    await t.show("indexed", s.workspaceSymbols("_main"));
});

test("inspect loads the databases above its inputs", async (t) => {
    await t.cli("inspect hover group-a/p1/main.cpp");
    await t.cli("inspect hover group-a/p2/main.cpp");
});

// Discovery looks no deeper than the root's direct subdirectories: the
// batch run, which opens no file, names the deeper databases instead.
test(
    "batch indexing names nested projects",
    { errors: ["Nothing to index: no compile_commands.json found"] },
    async (t) => {
        await t.cli("index --jobs 2");
    },
);
