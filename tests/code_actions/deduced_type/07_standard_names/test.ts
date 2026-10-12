import { answers } from "@clice/tools/testing";

// The standard names come from an included header, as in any real file; the
// server compiles that header into its preamble, clice inspect does not.
answers(async (a) => {
    for (const m of a.markers()) {
        await a.show(m.name, a.codeActions(m, { compile: true }));
    }
});
