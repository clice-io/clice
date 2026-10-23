import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// config.h holds directives only and compiles in main.cpp's context. A
// completion parses up to its point: one on a line typed after the
// directives keeps the compile's PCH and sees what they define, while the
// edit rebuilds it once for the compile.
test("completion reuses the pch", async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    const builds = async () => (await s.counts()).files["config.h"]?.pch ?? 0;
    expect(await s.errors("config.h")).toEqual([]);
    await s.sync();
    await t.show("builds after open", builds());
    s.edit("config.h", { after: "#define LIMIT 2\n", insert: "#ifdef LIM\n#endif\n" });
    await t.show("macros", s.completion(t.at("config.h", "#ifdef LIM|")));
    await s.sync();
    await t.show("builds after edit and completion", builds());
});
