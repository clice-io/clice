import { answers } from "@clice/tools/testing";

// The class's name calling its constructor is the class's own, renamed with
// it from either; an alias's and a class's inheriting the constructor are
// not.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("base", a.rename(a.marker("base"), "Root", { compile: true }));
    await a.show("call", a.rename(a.marker("call"), "Root", { compile: true }));
});
