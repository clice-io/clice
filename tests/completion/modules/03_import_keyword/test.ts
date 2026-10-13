import { answers } from "@clice/tools/testing";

// `import` completes at file scope of a module implementation unit as it does
// in an interface, up to the unit's first declaration; the module's own names
// complete there.
answers("keyword", async (a) => {
    await a.show("keyword", a.completion(a.marker("keyword")));
    await a.show("after_declaration", a.completion(a.marker("after_declaration")));
});

answers(
    "module names",
    { paths: { server: "clice inspect compiles one file alone, and main.cpp imports app" } },
    (a) => a.show("declared", a.completion(a.marker("declared"))),
);
