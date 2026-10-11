import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not format" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.formatting(m));
    }
});
