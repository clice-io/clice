import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("prepare", a.prepareRename(a.marker("compute")));
    await a.show("rename", a.rename(a.marker("compute"), "evaluate"));
});
