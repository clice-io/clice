import { test } from "@clice/tools/testing";

test(async (t) => {
    await t.cli("index --workers 2");
    // A parameter is known to its file alone: a name finds it with `path:`.
    await t.cli("refactor rename --symbol length --to size --dry-run");
    await t.cli('refactor rename --symbol "length path:main.cpp" --to size --dry-run');
});
