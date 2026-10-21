import * as fs from "node:fs";
import * as path from "node:path";
import { expect } from "vitest";
import { lmdb, test, type TestContext } from "@clice/tools/testing";

function database(t: TestContext): { file: string; bytes: Buffer } {
    const databases = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(databases).toHaveLength(1);
    const file = databases.join("");
    return { file, bytes: fs.readFileSync(path.join(t.root, file)) };
}

/// The leaf pages holding nothing but symbol records ('Y' keys), by key.
function symbolPages(bytes: Buffer): lmdb.Leaf[] {
    return lmdb
        .leaves(bytes)
        .filter(
            (leaf) => leaf.nodes.length > 1 && leaf.nodes.every((node) => node.key.startsWith("Y")),
        )
        .sort((lhs, rhs) => ((lhs.nodes[0]?.key ?? "") < (rhs.nodes[0]?.key ?? "") ? -1 : 1));
}

// A bad disk garbles a page of symbol records so that it lists its keys out
// of order. Lookups of other records read fine; the walk over the symbols a
// run's summary takes meets the damage, and the run says so and discards
// the database, which the next run builds again.
test("damaged page", { errors: ["The index database at .* is damaged"] }, async (t) => {
    await t.cli("index");
    const { file, bytes } = database(t);
    // Not the first such page: the walk reaches it from the page before.
    const [first, second] = symbolPages(bytes)[1]?.nodes ?? [];
    expect(second).toBeDefined();
    t.disk.corrupt(file, (page) => {
        const a = first?.pointer ?? 0;
        const b = second?.pointer ?? 0;
        const pointer = page.readUInt16LE(a);
        page.writeUInt16LE(page.readUInt16LE(b), a);
        page.writeUInt16LE(pointer, b);
    });
    await t.cli("index");
    await t.cli("index");
    await t.cli("query symbol-search f1500");
});

// A bad disk flips the bytes of every symbol record: a query reading one
// says it is damaged, and since only a writer meeting the damage discards
// the database, names the directory to delete.
test(
    "damaged records",
    { errors: ["The index database's record .* fails its checks"] },
    async (t) => {
        await t.cli("index");
        const { file, bytes } = database(t);
        const values = lmdb
            .leaves(bytes)
            .flatMap((leaf) => leaf.nodes)
            .filter((node) => node.key.startsWith("Y"));
        expect(values.length).toBeGreaterThan(0);
        t.disk.corrupt(file, (page) => {
            for (const node of values) {
                for (let i = node.value; i < node.value + node.size; i += 1) {
                    page[i] = (page[i] ?? 0) ^ 0xff;
                }
            }
        });
        await t.cli("query symbol-search f1500");
    },
);
