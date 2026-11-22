import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

const paths = { server: "what the file does not include yet comes from the server's index" };
answers("default", { paths, indexing: true }, items);
answers(
    "angled",
    { paths, indexing: true, config: { code_completion: { angled_includes: ["**"] } } },
    items,
);
answers(
    "quoted",
    { paths, indexing: true, config: { code_completion: { quoted_includes: ["**"] } } },
    items,
);
answers(
    "both",
    {
        paths,
        indexing: true,
        config: {
            code_completion: { angled_includes: ["**"], quoted_includes: ["widgets/**"] },
        },
    },
    items,
);
