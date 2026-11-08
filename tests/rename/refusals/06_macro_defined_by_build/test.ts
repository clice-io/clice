import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("size", a.rename(a.marker("size"), "BUFFER_BYTES"));
});
