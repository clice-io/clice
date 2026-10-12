import { answers, type AnswersContext } from "@clice/tools/testing";

async function highlights(a: AnswersContext, file: string): Promise<void> {
    for (const m of a.markers().filter((marker) => marker.path === file)) {
        await a.show(m.name, a.highlights(m));
    }
}

answers("interface", async (a) => {
    await highlights(a, "shapes.cppm");
});

answers(
    "importer",
    { paths: { server: "clice inspect compiles one file alone, and main.cpp imports shapes" } },
    async (a) => {
        await highlights(a, "main.cpp");
    },
);
