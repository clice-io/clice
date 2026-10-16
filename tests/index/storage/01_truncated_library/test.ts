import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The index library cut short, as a crash mid-write leaves it: a reader
// cannot repair it and says so instead of reading past its end, and the
// next `clice index` rebuilds it.
async function damaged(t: TestContext, cut: (library: string) => number): Promise<void> {
    await t.cli("index");
    const libraries = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(libraries).toHaveLength(1);
    const library = libraries.join("");
    t.disk.truncate(library, cut(library));
    await t.cli("query symbol-search add");
    await t.cli("index");
    await t.cli("query symbol-search add");
}

// Only the two meta pages are left, so the pages they declare end past the
// end of the file. A page is the system's (16 KiB on Apple silicon): the
// first meta page records it in its free database's pad, past the page
// header, the magic, the version, the address and the map size.
test("cut after the meta pages", async (t) => {
    await damaged(t, (library) => 2 * fs.readFileSync(path.join(t.root, library)).readUInt32LE(40));
});

test("emptied", async (t) => {
    await damaged(t, () => 0);
});
