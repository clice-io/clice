import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The build writes build/compile_commands.json and the user links it into
// the root, as clangd's setup suggests: one database under two spellings.

test(
    "linked root database",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.database({ "main.cpp": [] }, "build");
        t.disk.symlink("compile_commands.json", "build/compile_commands.json");
        const s = await t.start();
        await t.show("before", s.errors("main.cpp"));
        // The build runs again and rewrites the file the link names.
        t.disk.database({ "main.cpp": ["-DFEATURE"] }, "build");
        expect(await s.poll("cdb", { force: false })).toBe(0);
        expect(await s.poll("cdb", { force: false })).toBe(1);
        expect(await s.errors("main.cpp")).toEqual([]);
    },
);

test(
    "removed link yields to the build",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.database({ "main.cpp": [] }, "build");
        t.disk.symlink("compile_commands.json", "build/compile_commands.json");
        const s = await t.start();
        await t.show("before", s.errors("main.cpp"));
        // The tracker meets both spellings of the one database.
        expect(await s.poll("cdb", { force: false })).toBe(0);
        // The link goes; the build writes its database again.
        t.disk.rm("compile_commands.json");
        t.disk.database({ "main.cpp": ["-DFEATURE"] }, "build");
        await t.show("polls", [
            await s.poll("cdb", { force: false }),
            await s.poll("cdb", { force: false }),
            await s.poll("cdb", { force: false }),
        ]);
        expect(await s.errors("main.cpp")).toEqual([]);
    },
);

test(
    "batch loads it once",
    { except: { win32: "a symlink on Windows needs a privilege the CI runners lack" } },
    async (t) => {
        t.disk.database({ "main.cpp": ["-DFEATURE"] }, "build");
        t.disk.symlink("compile_commands.json", "build/compile_commands.json");
        await t.cli("index");
    },
);
