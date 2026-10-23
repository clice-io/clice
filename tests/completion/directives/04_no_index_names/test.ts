import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m));
    }
}

// A directive's line names no namespace's member: the index's names, more
// than a reply lists here, are not asked there, and its reply is whole.
answers(
    {
        paths: { server: "the names of other namespaces come from the server's index" },
        indexing: true,
    },
    items,
);
