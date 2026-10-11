import { expect } from "vitest";
import { test, type TestContext } from "@clice/tools/testing";

// The index library cut short, as a crash mid-write leaves it: a reader
// cannot repair it and says so instead of reading past its end, and the
// next `clice index` rebuilds it.
async function damaged(t: TestContext, bytes: number): Promise<void> {
    await t.cli("index");
    const libraries = t.disk.list(".clice/cache/*/index/*/index.mdb");
    expect(libraries).toHaveLength(1);
    t.disk.truncate(libraries.join(""), bytes);
    await t.cli("query --method symbolSearch --symbol add");
    await t.cli("index");
    await t.cli("query --method symbolSearch --symbol add");
}

// Its pages end past the end of the file; the message counts them in bytes.
test(
    "cut mid-page",
    { except: { darwin: "the message counts LMDB pages, 16 KiB there, so its sizes differ" } },
    async (t) => {
        await damaged(t, 8192);
    },
);

test("emptied", async (t) => {
    await damaged(t, 0);
});
