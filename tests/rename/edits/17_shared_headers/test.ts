import { answers } from "@clice/tools/testing";

// A header's internal function is one in every unit including it, and a
// header named without an extension is edited like another.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("helper", a.rename(a.marker("helper"), "assist", { compile: true }));
});
