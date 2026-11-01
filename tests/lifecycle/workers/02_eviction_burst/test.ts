import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// A pool that keeps two ASTs, asked about six documents at once: the burst
// compiles each once and answers every request, and the evicted documents
// come back on demand.

const UNITS = generate.units(6);

test(async (t) => {
    t.disk.write(UNITS);
    // Without the index, whose entries would answer the hovers, only a compile
    // can.
    const s = await t.start({
        config: { project: { enable_indexing: false } },
        env: { CLICE_TEST_MAX_RESIDENT: "2" },
    });
    const files = Object.keys(UNITS);
    for (const file of files) {
        s.open(file, { pull: false });
    }
    await t.show("burst", Object.fromEntries(files.map((file) => [file, s.semanticTokens(file)])));
    // Asked again one by one, each evicted document compiles again.
    const hovers: Record<string, Promise<unknown>> = {};
    for (const file of files) {
        await s.diagnostics(file);
        hovers[file] = s.hover(t.at(file, "int |unit_"));
        await hovers[file];
    }
    await t.show("asked again", hovers);
    // A request holds its document from its compile to its answer: each
    // compiles once for the burst, and once more when asked again after it
    // was evicted.
    expect((await s.counts()).compile).toBeLessThanOrEqual(2 * files.length);
});
