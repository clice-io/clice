import { answers } from "@clice/tools/testing";

// A category renamed to another of its class's would declare that one twice.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    const rename = (to: string) => a.rename(a.marker("extras"), to, { compile: true });
    await a.show("to_sibling", rename("Sizing"));
    await a.show("to_free_name", rename("Motion"));
});
