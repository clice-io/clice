import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("parameter", a.rename(a.marker("parameter"), "ratio", { compile: true }));
    await a.show("local", a.rename(a.marker("local"), "doubled", { compile: true }));
});
