import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A bad disk garbles a page of symbol records so that it lists its keys out
// of order. Lookups of other records read fine; the walk over the symbols a
// run's summary takes meets the damage, and the run says so and discards
// the database, which the next run builds again.
test({ errors: ["The index database at .* is damaged"] }, async (t) => {
    await t.cli("index");
    const databases = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(databases).toHaveLength(1);
    const database = databases.join("");
    // The first meta page records the page size past the page header, the
    // magic, the version, the address and the map size. A page starts with
    // its number, padding, flags (2 for a leaf) and the end of its node
    // pointers, which follow; a node is four 16-bit fields, its key size
    // last, then the key, whose first byte names the record's kind ('Y' for
    // symbols).
    const bytes = fs.readFileSync(path.join(t.root, database));
    const size = bytes.readUInt32LE(40);
    const symbols: { first: string; page: number }[] = [];
    for (let page = 2 * size; page + size <= bytes.length; page += size) {
        if ((bytes.readUInt16LE(page + 10) & 0x07) !== 0x02) {
            continue;
        }
        const nodes = (bytes.readUInt16LE(page + 12) - 16) / 2;
        const keys: string[] = [];
        for (let i = 0; i < nodes; i += 1) {
            const node = page + bytes.readUInt16LE(page + 16 + 2 * i);
            keys.push(bytes.toString("latin1", node + 8, node + 8 + bytes.readUInt16LE(node + 6)));
        }
        if (keys.length > 1 && keys.every((key) => key.startsWith("Y"))) {
            symbols.push({ first: keys[0] ?? "", page });
        }
    }
    symbols.sort((lhs, rhs) => (lhs.first < rhs.first ? -1 : 1));
    // Not the first such page: the walk reaches it from the page before.
    const damaged = symbols[1]?.page;
    expect(damaged).toBeDefined();
    t.disk.corrupt(database, (page) => {
        const at = (damaged ?? 0) + 16;
        const first = page.readUInt16LE(at);
        page.writeUInt16LE(page.readUInt16LE(at + 2), at);
        page.writeUInt16LE(first, at + 2);
    });
    await t.cli("index");
    await t.cli("index");
    await t.cli("query symbol-search f1500");
});
