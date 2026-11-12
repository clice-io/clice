import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("parameter", a.rename(a.marker("parameter"), "ratio", { compile: true }));
    await a.show("local", a.rename(a.marker("local"), "doubled", { compile: true }));
    // The name another declaration gives the parameter is no clash.
    await a.show("defined", a.rename(a.marker("defined"), "first", { compile: true }));
});
