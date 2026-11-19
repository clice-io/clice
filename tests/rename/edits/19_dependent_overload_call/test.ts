import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    // The only instantiation resolves the dependent call to this overload.
    await a.show("rename", a.rename(a.marker("start"), "launch", { compile: true }));
});
