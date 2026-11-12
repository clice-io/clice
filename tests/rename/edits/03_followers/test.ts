import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("class", a.rename(a.marker("class"), "Polygon", { compile: true }));
    await a.show("constructor", a.rename(a.marker("constructor"), "Tile", { compile: true }));
    await a.show("virtual", a.rename(a.marker("virtual"), "surface", { compile: true }));
    await a.show("template", a.rename(a.marker("template"), "Holder", { compile: true }));
    await a.show("guide", a.rename(a.marker("guide"), "Holder", { compile: true }));
    await a.show(
        "function_template",
        a.rename(a.marker("function_template"), "choose", { compile: true }),
    );
    await a.show("destructor", a.prepareRename(a.marker("destructor")));
});
