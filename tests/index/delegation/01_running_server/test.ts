import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A running server holds the index's writer lock and leaves a record of
// how to reach it (.clice/server.json); the commands that write the index
// ask it to do the work instead.
test("asks the running server to index", async (t) => {
    const s = await t.start();
    await s.indexed();
    expect(t.disk.list(".clice/server.json")).toEqual([".clice/server.json"]);
    await t.cli("index --jobs 2");
    await t.cli("index --json");
    t.disk.edit("main.cpp", { before: "int main()", insert: "int extra() { return 7; }\n" });
    await t.cli("query symbol-search extra --fresh");
    // What the server indexed for the fresh query is on disk for every
    // reader after it.
    await t.cli("query symbol-search extra");
    await s.stop();
    expect(t.disk.list(".clice/server.json")).toEqual([]);
});

// A lock holder without a record (a batch run, a server of another build)
// cannot be asked: the commands that need the writer give up, while reads
// go on from the disk, which the finished round saved.
test("refuses a writer it cannot ask", async (t) => {
    const s = await t.start();
    await s.indexed();
    t.disk.rm(".clice/server.json");
    await t.cli("index");
    await t.cli("query symbol-search compute --fresh");
    await t.cli("query symbol-search compute");
});
