import { answers } from "@clice/tools/testing";

answers({ paths: { server: "clice inspect does not rename" }, indexing: true }, async (a) => {
    await a.show("instantiated", a.rename(a.marker("circle"), "perimeter"));
    await a.show("uninstantiated", a.rename(a.marker("box"), "volume"));
});
