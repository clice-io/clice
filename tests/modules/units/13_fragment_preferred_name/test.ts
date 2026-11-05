import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// main.cppm's PCH and the lib module both declare basic_string_view<char>,
// both from <string_view>. With preferred_name kept in the PCH, reading its
// specialization reads the string_view typedef through the attribute before
// the specialization merges with lib's, and the typedef's type stays apart
// from the one sink takes (llvm/llvm-project#56490).
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("lib.cppm")).toEqual([]);
    expect(await s.errors("main.cppm")).toEqual([]);
    await s.sync();
    expect((await s.counts()).files["main.cppm"]?.pch).toBe(1);
});
