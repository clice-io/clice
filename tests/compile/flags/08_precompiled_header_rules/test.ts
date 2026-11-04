import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// A rule edits the commands of the target's units but not that of the unit
// writing its PCH: a PCH under the unit's edits would be no unit's compile,
// which nothing indexes, so the unit's own preamble PCH holds the header,
// built under its edits — diameter() exists only under the rule's define.

test("a rule's define reaches the pch", async (t) => {
    const s = await t.start({
        config: {
            project: { enable_indexing: false },
            rules: [{ patterns: ["src/**"], append: ["-DSHAPES_METRIC"] }],
        },
    });
    await t.show("main.cpp", s.diagnostics("src/main.cpp"));
    await t.show("width", s.definition(t.marker("width")));
    const { files } = await s.counts();
    expect({
        writer: files["build/CMakeFiles/app.dir/cmake_pch.hxx.cxx"]?.pch ?? 0,
        main: files["src/main.cpp"]?.pch,
    }).toEqual({ writer: 0, main: 1 });
});
