import { test } from "@clice/tools/testing";

// A `path:` filter is a path from the workspace root, `./` or not, a file
// name, or an absolute path.
test(async (t) => {
    await t.cli("index");
    for (const text of [
        "sum path:src/",
        '"tally" path:src/a.cpp',
        '"tally" path:./lib/src/b.cpp',
        '"tally" path:b.cpp',
        `sum path:"${t.root}/lib"`,
    ]) {
        await t.cli(["query", "--method", "symbolSearch", "--symbol", text]);
    }
});
