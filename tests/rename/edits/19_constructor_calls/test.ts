import { answers } from "@clice/tools/testing";

// A name calling a constructor renames as the type it writes: the class's
// own name is the class's, renamed with it from either; an alias's, also in
// a template, and a class's inheriting the constructor are theirs.
answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("base", a.rename(a.marker("base"), "Root", { compile: true }));
    await a.show("call", a.rename(a.marker("call"), "Root", { compile: true }));
    await a.show("alias_call", a.rename(a.marker("alias_call"), "Handle", { compile: true }));
    await a.show("derived_call", a.rename(a.marker("derived_call"), "Child", { compile: true }));
    await a.show(
        "alias_template",
        a.rename(a.marker("alias_template"), "Legacy", { compile: true }),
    );
});
