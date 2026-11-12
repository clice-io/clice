import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.prepareRename(m));
    }
});
