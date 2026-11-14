import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { lmdb, test } from "@clice/tools/testing";

// A database another version of clice wrote, its schema in the record that
// describes the file: a reader says the database is of another version and
// must be rebuilt, which the next `clice index` does.
test(async (t) => {
    await t.cli("index");
    const databases = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(databases).toHaveLength(1);
    const file = databases.join("");
    const bytes = fs.readFileSync(path.join(t.root, file));
    const meta = lmdb
        .leaves(bytes)
        .flatMap((leaf) => leaf.nodes)
        .find((node) => node.key === "\xffmeta");
    expect(meta).toBeDefined();
    // The schema leads the record.
    t.disk.corrupt(file, (database) => {
        database.writeUInt32LE(1, meta?.value ?? 0);
    });
    await t.cli("query symbol-search add");
    await t.cli("index");
    await t.cli("query symbol-search add");
});
