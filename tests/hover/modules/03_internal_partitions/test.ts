import { answers } from "@clice/tools/testing";

answers("partition", async (a) => {
    await a.show("declared", a.hover(a.marker("declared")));
});

answers(
    "importer",
    { paths: { server: "clice inspect compiles one file alone, and main.cpp imports :detail" } },
    async (a) => {
        await a.show("use", a.hover(a.marker("use")));
    },
);
