import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("size", a.rename(a.marker("size"), "BUFFER_BYTES"));
    await a.show("limit", a.rename(a.marker("limit"), "MAX_LIMIT"));
    await a.show("argument", a.rename(a.marker("twice"), "DOUBLE"));
});
