import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const at = a.marker("compute");
    for (const name of ["1abc", "class", "Point", "TWICE", "other", "_Compute"]) {
        await a.show(name, a.rename(at, name));
    }
});
