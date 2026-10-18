import { answers, test } from "@clice/tools/testing";

// The server answers a references request with `references.limit`
// locations at most, those in the file it is asked from first; `clice
// query` lists every one.
answers({ paths: { server: "the limit is the server's" }, indexing: true }, async (a) => {
    await a.show("references", a.references(a.marker("use")));
});

test("query", async (t) => {
    await t.cli("index");
    await t.cli("query references main.cpp:4:12");
});
