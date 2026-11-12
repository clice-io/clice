import { expect } from "vitest";
import { generate, test } from "@clice/tools/testing";

// A pool that keeps two ASTs, asked about six documents at once: the burst
// compiles each once and answers every request, and the evicted documents
// come back on demand.

const UNITS = generate.units(6);

test(async (t) => {
    t.disk.write(UNITS);
    // Without the index, whose entries would answer the burst's tokens, only
    // a compile can.
    const s = await t.start({
        config: { project: { enable_indexing: false, max_resident_documents: 2 } },
    });
    const files = Object.keys(UNITS);
    /// How many times each file compiled, once the server is idle.
    const compiles = async (): Promise<number[]> => {
        await s.sync();
        const { files: built } = await s.counts();
        return files.map((file) => built[file]?.compile ?? 0);
    };
    for (const file of files) {
        s.open(file, { pull: false });
    }
    await t.show("burst", Object.fromEntries(files.map((file) => [file, s.semanticTokens(file)])));
    // A request holds its document from its compile to its answer: the
    // burst compiles each once, though the pool keeps only two.
    expect(await compiles()).toEqual(files.map(() => 1));
    // Asked again one by one, each evicted document compiles again.
    const hovers: Record<string, Promise<unknown>> = {};
    for (const file of files) {
        await s.diagnostics(file);
        hovers[file] = s.hover(t.at(file, "int |unit_"));
        await hovers[file];
    }
    await t.show("asked again", hovers);
    // An evicted document compiles once more, one still kept not at all:
    // which two the burst left kept depends on the order its compiles
    // finished in, so a file compiled once or twice, never more.
    expect((await compiles()).filter((count) => count !== 1 && count !== 2)).toEqual([]);
});
