import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const at = a.marker("limit");
    for (const name of ["count", "OTHER", "FEATURE_X", "defined", "_Limit"]) {
        await a.show(name, a.rename(at, name));
    }
});
