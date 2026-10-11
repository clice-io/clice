import { test } from "@clice/tools/testing";

// The .clang-tidy appears where the lookup found none, then changes.
test(async (t) => {
    const s = await t.start();
    await t.show("none", s.diagnostics("src/main.cpp"));
    t.disk.write(".clang-tidy", 'Checks: "-*,modernize-use-nullptr"\n');
    await s.poll("workspace");
    await t.show("created", s.diagnostics("src/main.cpp"));
    const created = t.disk.mtime(".clang-tidy");
    t.disk.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    t.disk.touch(".clang-tidy", new Date(created.getTime() + 10_000));
    await s.poll("workspace");
    await t.show("edited", s.diagnostics("src/main.cpp"));
});
