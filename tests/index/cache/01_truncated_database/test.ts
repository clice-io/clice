import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The index database cut short, as a crash mid-write leaves it: a reader
// cannot repair it and says so instead of reading past its end, and the
// next `clice index` rebuilds it.
async function damaged(t: TestContext, cut: (database: string) => number): Promise<void> {
    await t.cli("index");
    const databases = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(databases).toHaveLength(1);
    const database = databases.join("");
    t.disk.truncate(database, cut(database));
    await t.cli("query symbol-search add");
    await t.cli("index");
    await t.cli("query symbol-search add");
}

// Only the two meta pages are left, so the pages they declare end past the
// end of the file. A page is the system's (16 KiB on Apple silicon): the
// first meta page records it in its free database's pad, past the page
// header, the magic, the version, the address and the map size.
test("cut after the meta pages", async (t) => {
    await damaged(t, (database) => 2 * fs.readFileSync(path.join(t.root, database)).readUInt32LE(40));
});

test("emptied", async (t) => {
    await damaged(t, () => 0);
});
