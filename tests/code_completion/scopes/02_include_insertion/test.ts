import { answers, type AnswersContext } from "@clice/tools/testing";

async function items(a: AnswersContext): Promise<void> {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
    }
}

const paths = { server: "what the file does not include yet comes from the server's index" };
answers("default", { paths, indexing: true }, items);
answers(
    "configured",
    { paths, indexing: true, config: { code_completion: { insert_includes: false } } },
    items,
);

// A macro the file reaches comes once, without an `#include`, whether or not
// items of one label are merged.
answers(
    "unbundled",
    { paths, indexing: true, config: { code_completion: { bundle_overloads: false } } },
    (a) => a.show("macro", a.completion(a.marker("macro")), { parts: ["insert", "edits"] }),
);

// A limited list is cut after the names without a header go: the one name
// the limit keeps is one that can be inserted.
answers("limited", { paths, indexing: true, config: { code_completion: { limit: 1 } } }, (a) =>
    a.show("function_call", a.completion(a.marker("function_call")), {
        parts: ["insert", "edits"],
    }),
);
