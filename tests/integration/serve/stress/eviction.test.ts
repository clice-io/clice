/// Worker document eviction: opening more files than a stateful worker's
/// LRU cap must not silently break features on the evicted documents.

import { at, expect, serve } from "../../fixtures.ts";

const FILE_COUNT = 18; // one stateful worker holds at most 16 compiled documents

const NAMES = Array.from(
    { length: FILE_COUNT },
    (_, i) => `file_${String(i).padStart(2, "0")}.cpp`,
);

// A single stateful worker so all opens land in one document cache.
serve.files(Object.fromEntries(NAMES.map((name, i) => [name, `int value_${i} = ${i};\n`])), {
    config: { project: { stateful_worker_count: 1 } },
})("evicted document recovers", async ({ s }) => {
    for (const name of NAMES) {
        await s.compiled(name);
    }

    // The first file was LRU-evicted from the worker; hover must trigger a
    // recompile instead of silently returning null against the lost AST.
    const hover = await s.hover(at(NAMES[0]!, "int |value_0"));
    expect(hover, "hover on the evicted document must recover").not.toBeNull();
});
