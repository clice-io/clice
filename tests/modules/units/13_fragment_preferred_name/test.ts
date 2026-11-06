import { expect } from "vitest";
import { test } from "@clice/tools/testing";

// The PCHs of main.cppm and user.cpp, and the lib module, all declare
// basic_string_view<char>, from <string_view>. With preferred_name kept in
// a PCH, reading its specialization reads the string_view typedef through
// the attribute before the specialization merges with lib's, and the
// typedef's type stays apart from the one sink takes
// (llvm/llvm-project#56490).
test(async (t) => {
    const s = await t.start({ config: { project: { enable_indexing: false } } });
    expect(await s.errors("lib.cppm")).toEqual([]);
    expect(await s.errors("main.cppm")).toEqual([]);
    expect(await s.errors("user.cpp")).toEqual([]);
    await s.sync();
    const { files } = await s.counts();
    expect([files["main.cppm"]?.pch, files["user.cpp"]?.pch]).toEqual([1, 1]);
});
