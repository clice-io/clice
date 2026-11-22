import { answers, type AnswersContext } from "@clice/tools/testing";

// The action spells the header as completion does: quotes for one -I finds,
// angle brackets for one -isystem finds, unless a pattern says otherwise.

async function actions(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.codeActions(m, { compile: true }));
    }
}

const paths = {
    server: "clice inspect resolves code actions as an empty index would: no header declares the name",
};
answers("default", { paths, indexing: true }, actions);
answers(
    "configured",
    {
        paths,
        indexing: true,
        config: {
            code_completion: { angled_includes: ["widgets/**"], quoted_includes: ["acme/**"] },
        },
    },
    actions,
);
