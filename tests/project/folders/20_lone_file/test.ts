import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { expect } from "vitest";
import { generate, test, type Serve } from "@clice/tools/testing";

/// The cache of the files outside every folder that `s` serves: its own
/// directory under clice's temporary one.
function cache(s: Serve): string {
    const user = process.platform === "win32" ? "clice" : `clice-${String(os.userInfo().uid)}`;
    return path.join(s.temp ?? os.tmpdir(), user, String(s.client.child.pid));
}

// A file outside every folder precompiles what it includes, as a folder's
// file does: what the header declares leads there.
test("definition in an included header", async (t) => {
    const s = await t.start({ folders: [] });
    expect(await s.errors("main.cpp")).toEqual([]);
    await t.show("helper", s.definition(t.at("main.cpp", "return |helper()")));
});

// The preamble's index is large enough to be mapped, which Windows deletes
// only once unmapped.
test("cache gone with the server", async (t) => {
    t.disk.write("big.h", generate.functions(3000));
    t.disk.write("big.cpp", '#include "big.h"\n\nint use() { return function_0000(); }\n');
    const s = await t.start({ folders: [] });
    expect(await s.errors("big.cpp")).toEqual([]);
    const dir = cache(s);
    expect((await s.stats()).pchCacheEntries).toBe(1);
    expect(fs.globSync("precompiled/*/*/pch/*.pch", { cwd: dir })).toHaveLength(1);
    await s.stop();
    expect(fs.existsSync(dir)).toBe(false);
});

// With no file outside the folders open, a folder added stops serving them,
// and the held build keeps that stopping: the next file outside them waits
// for it, then opens the cache afresh.
test("next file waits for the cache", async (t) => {
    const s = await t.start({ folders: [] });
    const build = await s.park("pch", "main.cpp", { at: "reply" });
    s.open("main.cpp");
    await build.reached();
    s.close("main.cpp");
    await s.changeFolders({ added: ["app"] });
    s.open("other.cpp");
    await build.release();
    await s.sync();
    expect(await s.errors("other.cpp")).toEqual([]);
    expect(fs.existsSync(cache(s))).toBe(true);
});

// clice's temporary directory cannot be made (a file sits where it goes):
// the files outside every folder are not served, and the user is told why.
test("temporary directory unusable", async (t) => {
    t.disk.write("tmp", "not a directory\n");
    const tmp = path.join(t.root, "tmp");
    const s = await t.start({ folders: [], env: { TMPDIR: tmp, TMP: tmp, TEMP: tmp } });
    expect(await s.errors("main.cpp")).toEqual([]);
    const guidance = (await s.guidance()).join("\n");
    expect(guidance).toContain("cannot hold clice's temporary files");
    expect(guidance).toContain("no file outside every workspace folder is served");
});
