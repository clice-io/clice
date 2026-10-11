import { expect } from "vitest";
import { test, type Serve } from "@clice/tools/testing";

async function pchBuilds(s: Serve): Promise<number | undefined> {
    await s.sync();
    return (await s.counts()).files["main.cpp"]?.pch;
}

test("missing header waits for its file", async (t) => {
    const s = await t.start();
    await t.show("missing", s.errors("main.cpp"));
    // Body edits reuse the PCH that failed: nothing it read has changed.
    for (const n of [1, 2, 3]) {
        s.edit("main.cpp", {
            replace: `generated() + ${String(n - 1)}`,
            with: `generated() + ${String(n)}`,
        });
        await s.errors("main.cpp");
    }
    expect(await pchBuilds(s)).toBe(1);
    // The header showing up where the include looked is a new input.
    t.disk.write("generated.h", "#pragma once\n\ninline int generated() {\n    return 1;\n}\n");
    expect(await s.errors("main.cpp")).toEqual([]);
    expect(await pchBuilds(s)).toBe(2);
});
