import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --jobs 2");
    // A parameter is known to its file alone: a name finds it with `path:`.
    await t.cli("refactor rename length size --dry-run");
    await t.cli('refactor rename "length path:main.cpp" size --dry-run');
});
