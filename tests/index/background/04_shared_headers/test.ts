import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The eight units including shapes.h share their command: their index runs
// load the PCH of a header clice writes for them, which the pass builds
// once, indexing it in the same compile, and drops. wide.cpp defines WIDE before the include, which the
// PCH did not see: it parses shapes.h itself. Without the shared header the
// index answers alike.
const MARKERS = ["meters", "area_type", "area", "widen"];

test(async (t) => {
    const s = await t.start();
    await s.indexed();
    const shared = Object.entries((await s.counts()).files).filter(([file]) =>
        file.includes("/shared/"),
    );
    expect(shared.map(([, counts]) => ({ pch: counts.pch, index: counts.index }))).toEqual([
        { pch: 1, index: 0 },
    ]);
    const answered: Record<string, unknown> = {};
    for (const name of MARKERS) {
        const references = s.references(t.marker(name));
        await t.show(name, references);
        answered[name] = await references;
    }
    await s.stop();

    const plain = await t.start({ config: { project: { shared_pch: false } } });
    await plain.indexed();
    for (const name of MARKERS) {
        expect(await plain.references(t.marker(name)), name).toEqual(answered[name]);
    }
});
