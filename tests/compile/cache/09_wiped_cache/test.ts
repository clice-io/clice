import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

/// What a user's delete of `target` removes under a running server: all of
/// it, but on Windows, which removes no file a process holds open, the
/// server's index database and the directories holding it stay.
function wipe(target: string): void {
    const directory = fs.lstatSync(target).isDirectory();
    if (directory) {
        for (const name of fs.readdirSync(target)) {
            wipe(path.join(target, name));
        }
    }
    try {
        if (directory) {
            fs.rmdirSync(target);
        } else {
            // Not fs.rmSync: on Windows, Node 26's ends the process when it
            // fails on a path the ANSI code page cannot represent.
            fs.unlinkSync(target);
        }
    } catch (error) {
        const code = (error as NodeJS.ErrnoException).code ?? "";
        if (process.platform !== "win32" || !["EPERM", "EBUSY", "ENOTEMPTY"].includes(code)) {
            throw error;
        }
    }
}

test("wiped while running", async (t) => {
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    // The user resets the cache without restarting the server; the next
    // PCH build creates the store's directories again.
    wipe(path.join(t.root, ".clice/cache"));
    expect(t.disk.list(".clice/cache/**/*.pch")).toEqual([]);
    t.disk.write("header.h", "#pragma once\n\nstruct W {\n    int x;\n    int y;\n};\n");
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(2);
    expect(t.disk.list(".clice/cache/*/pch/*.pch")).toHaveLength(1);
});
