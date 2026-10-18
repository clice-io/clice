import { answers, test } from "@clice/tools/testing";

// The server answers a references request with `references.limit`
// locations at most, keeping those in the file it is asked from when it
// cuts the list short; a limit of 0 and `clice query` list every one.
answers("server", { paths: { server: "the limit is the server's" }, indexing: true }, async (a) => {
    await a.show("references", a.references(a.marker("use")));
});

answers(
    "no limit",
    {
        paths: { server: "the limit is the server's" },
        indexing: true,
        config: { references: { limit: 0 } },
    },
    async (a) => {
        await a.show("references", a.references(a.marker("use")));
    },
);

test("query", async (t) => {
    await t.cli("index");
    await t.cli("query references main.cpp:4:12");
});
