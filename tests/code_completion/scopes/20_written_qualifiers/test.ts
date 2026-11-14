import { answers } from "@clice/tools/testing";

// A name the file does not include yet completes after the scope written
// before it, through the header declaring it. The index is asked for the
// scope as written: after a macro or a namespace alias, nothing it holds.
answers(
    {
        paths: { server: "what the file does not include yet comes from the server's index" },
        indexing: true,
    },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.completion(m), { parts: ["insert", "edits"] });
        }
    },
);
