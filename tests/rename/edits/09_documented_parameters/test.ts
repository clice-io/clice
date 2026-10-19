import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("parameter", a.rename(a.marker("parameter"), "ratio", { compile: true }));
    await a.show("comment", a.rename(a.marker("comment"), "extra", { compile: true }));
    await a.show("template", a.rename(a.marker("template"), "U", { compile: true }));
});
