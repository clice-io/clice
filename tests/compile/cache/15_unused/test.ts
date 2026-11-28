import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The project keeps its PCHs and PCMs in `shared`; a server with a cache
// directory of its own stands for another project there.

const DAY = 24 * 3600 * 1000;

/// The directory in `shared` each project keeps its PCHs and PCMs in, named
/// by a cache key.
function projects(t: TestContext): string[] {
    return t.disk
        .list("shared/*/store.lock")
        .map((lock) => path.posix.dirname(lock))
        .filter((dir) => /^shared\/[0-9a-f]{32}$/.test(dir));
}

/// Make the project in `dir` look last used 16 days ago: its list of
/// entries written then, its last server's `tmp/<pid>` gone then.
function age(t: TestContext, dir: string): void {
    const then = new Date(Date.now() - 16 * DAY);
    for (const manifest of t.disk.list(`${dir}/*/manifest.json`)) {
        t.disk.touch(manifest, then);
        t.disk.touch(path.posix.join(path.posix.dirname(manifest), "tmp"), then);
    }
}

async function other(t: TestContext): Promise<string> {
    const s = await t.start({ cacheDir: ".clice-other" });
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.stop();
    const [dir] = projects(t);
    return dir ?? "";
}

test("an entry unused for 15 days goes", async (t) => {
    const first = await t.start();
    expect(await first.errors("main.cpp")).toEqual([]);
    await first.stop();
    // Its list of entries says the PCH was last used in 1970.
    const [manifest] = t.disk.list("shared/*/*/manifest.json");
    const entries = JSON.parse(t.disk.read(manifest ?? "")) as { entries: { atime: number }[] };
    for (const entry of entries.entries) {
        entry.atime = 1;
    }
    t.disk.write(manifest ?? "", JSON.stringify(entries));
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    await s.sync();
    expect((await s.counts()).pch).toBe(1);
});

test("a project unused for 15 days goes", async (t) => {
    age(t, await other(t));
    // What no clice named stays, whatever its name and files.
    t.disk.write("shared/0123456789abcdef0123456789abcdef/notes.txt", "mine\n");
    t.disk.write("shared/not-a-project/store.lock", "");
    t.disk.write("shared/not-a-project/v1/notes.txt", "mine\n");
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(projects(t)).toHaveLength(1);
    expect(t.disk.list("shared/0123456789abcdef0123456789abcdef/notes.txt")).toHaveLength(1);
    expect(t.disk.list("shared/not-a-project/v1/notes.txt")).toHaveLength(1);
});

test("a project used lately stays", async (t) => {
    const dir = await other(t);
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(projects(t)).toHaveLength(2);
    expect(projects(t)).toContain(dir);
});

test("what an earlier clice left beside the index goes", async (t) => {
    const first = await t.start();
    expect(await first.errors("main.cpp")).toEqual([]);
    await first.stop();
    const [layout] = t.disk
        .list(".clice/cache/*/index/*/index.mdb")
        .map((database) => path.posix.dirname(path.posix.dirname(path.posix.dirname(database))));
    const left = ["pch/old.pch", "pch/old.pch.idx", "pcm/M-old.pcm"];
    for (const file of left) {
        t.disk.write(`${layout ?? ""}/${file}`, "an earlier clice's\n");
    }
    t.disk.write(`${layout ?? ""}/manifest.json`, '{"entries":[]}');
    const s = await t.start();
    expect(await s.errors("main.cpp")).toEqual([]);
    for (const file of [...left, "manifest.json"]) {
        expect(t.disk.list(`${layout ?? ""}/${file}`)).toEqual([]);
    }
});
