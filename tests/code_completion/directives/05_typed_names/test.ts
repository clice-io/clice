import { answers } from "@clice/tools/testing";

// The name the cursor is in is replaced whole, however far it is spaced from
// its `#`.
answers(
    { paths: { server: "directive names are answered before anything compiles" } },
    async (a) => {
        for (const m of a.markers()) {
            await a.show(m.name, a.completion(m), { parts: ["insert", "edit"] });
        }
    },
);
