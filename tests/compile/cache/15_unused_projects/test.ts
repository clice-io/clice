import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The projects of the shared `precompiled_dir`: a server with a cache
// directory of its own stands for another project there.

/// The directory in `shared/` each project keeps its PCHs and PCMs in.
function projects(t: TestContext): string[] {
    return t.disk.list("shared/*/store.lock").map((lock) => path.dirname(lock));
}

/// Make the project in `dir` look last used 16 days ago: its list of
/// entries written then, its last server's `tmp/<pid>` gone then.
function age(t: TestContext, dir: string): void {
    const then = new Date(Date.now() - 16 * 24 * 3600 * 1000);
    for (const layout of fs.readdirSync(path.join(t.root, dir))) {
        for (const used of ["manifest.json", "tmp"]) {
            const file = path.join(t.root, dir, layout, used);
            if (fs.existsSync(file)) {
                fs.utimesSync(file, then, then);
            }
        }
    }
}

async function other(t: TestContext): Promise<string> {
    const s = await t.start({ cacheDir: ".clice-other" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.stop();
    const [dir] = projects(t);
    return dir ?? "";
}

test("a project unused for 15 days goes", async (t) => {
    age(t, await other(t));
    // Another program's directory stays, whatever its name.
    t.disk.write("shared/0123456789abcdef0123456789abcdef/notes.txt", "mine\n");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(projects(t)).toHaveLength(1);
    expect(t.disk.list("shared/0123456789abcdef0123456789abcdef/notes.txt")).toHaveLength(1);
});

test("a project used lately stays", async (t) => {
    const dir = await other(t);
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(projects(t)).toHaveLength(2);
    expect(projects(t)).toContain(dir);
});
