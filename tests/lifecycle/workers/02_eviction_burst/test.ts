import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// A worker that holds two documents at most, asked about six at once:
// the burst settles instead of evicting compiles in flight and compiling
// them again without end, answers every request, and the evicted documents
// come back on demand.

const UNITS = generate.units(6);

test(async (t) => {
    t.disk.write(UNITS);
    // Without the index, whose entries would answer the hovers, only a compile
    // can.
    const s = await t.start({
        config: { project: { enable_indexing: false } },
        env: { CLICE_TEST_MAX_DOCUMENTS: "2" },
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
    // A document compiles once for the burst, once more for each eviction
    // that caught it between its compile and its query, and once when asked
    // again; each such eviction takes another document landing in that
    // window, so they stay few. A loop of evictions runs far past this.
    expect((await s.counts()).compile).toBeLessThanOrEqual(5 * files.length);
});
