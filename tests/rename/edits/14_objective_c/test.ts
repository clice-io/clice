import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("class", a.rename(a.marker("interface"), "Figure"));
    await a.show("protocol", a.rename(a.marker("protocol"), "Paintable"));
    await a.show("ivar", a.rename(a.marker("ivar_declaration"), "_moves"));
    await a.show("category", a.rename(a.marker("category"), "Motion"));
});
