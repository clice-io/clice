import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// config.h holds directives only and compiles in main.cpp's context; a
// header with a declaration after its directives keeps one PCH here.
test("completion reuses the pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("config.h")).toEqual([]);
    await s.sync();
    await t.show("builds after open", (await s.counts()).files["config.h"]?.pch ?? 0);
    await s.completion(t.at("config.h", "#define LIMIT |2"));
    await s.sync();
    await t.show("builds after completion", (await s.counts()).files["config.h"]?.pch ?? 0);
    s.edit("config.h", { after: "#define LIMIT 2\n", insert: "#define OTHER 3\n" });
    expect(await s.errors("config.h")).toEqual([]);
    await s.completion(t.at("config.h", "#define OTHER |3"));
    await s.sync();
    await t.show(
        "builds after edit and completion",
        (await s.counts()).files["config.h"]?.pch ?? 0,
    );
});
