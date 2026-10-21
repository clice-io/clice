import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A bad disk zeroes the pages holding nothing but symbol records. Lookups
// of other records read fine; the walk over the symbols a run's summary
// takes crosses the damage, and the run says so and discards the database,
// which the next run builds again.
test({ errors: ["The index database at .* is damaged"] }, async (t) => {
    await t.cli("index");
    const databases = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(databases).toHaveLength(1);
    const database = databases.join("");
    const bytes = fs.readFileSync(path.join(t.root, database));
    // The first meta page records the page size past the page header, the
    // magic, the version, the address and the map size. A page starts with
    // its number, padding, flags (2 for a leaf) and the end of its node
    // pointers; a node is four 16-bit fields, its key size last, then the
    // key, whose first byte names the record's kind ('Y' for symbols).
    const size = bytes.readUInt32LE(40);
    const pages: [number, number][] = [];
    for (let page = 2 * size; page + size <= bytes.length; page += size) {
        if ((bytes.readUInt16LE(page + 10) & 0x07) !== 0x02) {
            continue;
        }
        const nodes = (bytes.readUInt16LE(page + 12) - 16) / 2;
        let symbols = true;
        for (let i = 0; i < nodes && symbols; i += 1) {
            const node = page + bytes.readUInt16LE(page + 16 + 2 * i);
            symbols = bytes.readUInt16LE(node + 6) > 0 && bytes[node + 8] === 0x59;
        }
        if (nodes > 0 && symbols) {
            pages.push([page, size]);
        }
    }
    expect(pages.length).toBeGreaterThan(0);
    t.disk.zero(database, pages);
    await t.cli("index");
    await t.cli("index");
    await t.cli("query symbol-search f1500");
});
