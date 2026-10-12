import { answers } from "@clice/tools/testing";

// The `delete` that runs the renamed class's destructor spells no name.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("widget", a.rename(a.marker("widget"), "Gadget", { compile: true }));
});
