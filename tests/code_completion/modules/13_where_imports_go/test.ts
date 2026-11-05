import { answers } from "@clice/tools/testing";

// conditional.cppm declares its module under a conditional, which the
// preprocessor reads with the file's command: its own module's partitions
// follow. A global module fragment takes no import: fragment.cppm's, and
// fragment_conditional.cppm's, whose declaration the conditional leaves out.
answers({ paths: { server: "import completion reads the server's module map" } }, async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.completion(m), { parts: ["insert", "edit"] });
    }
});
