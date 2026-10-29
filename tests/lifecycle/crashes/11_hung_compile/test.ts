import { test } from "@clice/tools/testing";

// A compile running past its deadline is taken for a hung worker: the
// worker is killed and the file stopped like a crash, so the hang does not
// come back on the next request.

test(
    {
        errors: [String.raw`\[anomaly:WorkerCrash\] |Last output of crashed worker`],
    },
    async (t) => {
        const s = await t.start({
            config: { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } },
            env: { CLICE_TEST_REQUEST_DEADLINE_MS: "2000" },
            // The deadline's kill names its request without a crash of the
            // worker's own: the note below is how it shows.
            anomalies: true,
        });
        const x = t.marker("x");
        s.open("main.cpp");
        await t.show("hover", s.hover(x));
        await t.show("pushes", s.pushes("main.cpp"));
        await t.show("hover asked again", s.hover(x));
        await t.show("pushes after", s.pushes("main.cpp"));
    },
);
