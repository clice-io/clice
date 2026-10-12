import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("base", a.rename(a.marker("base"), "Figure", { compile: true }));
});
