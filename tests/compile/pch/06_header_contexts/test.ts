import { expect } from "vitest";
import { test } from "@clice/tools/testing";

test("prefixes are precompiled", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("main.cpp")).toEqual([]);
    // Each header compiles after what main.cpp has before it. utils.h
    // needs the definition there, which the PCH carries alone: compiled a
    // second time it would be a redefinition.
    expect(await s.errors("utils.h")).toEqual([]);
    // errors.def has no directives of its own, but its prefix does.
    expect(await s.errors("errors.def")).toEqual([]);
    // Nothing comes before bare.h: there is nothing to precompile.
    expect(await s.errors("bare.h")).toEqual([]);
    await s.sync();
    const { files } = await s.counts();
    expect({
        "utils.h": files["utils.h"]?.pch,
        "errors.def": files["errors.def"]?.pch,
        "bare.h": files["bare.h"]?.pch,
    }).toEqual({ "utils.h": 1, "errors.def": 1, "bare.h": 0 });
});
