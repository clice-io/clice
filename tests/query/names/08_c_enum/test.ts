import { test } from "@clice/tools/testing";

// In C an enumerator has no qualified name to tie its uses together across
// sources; references reach every file all the same.
test(async (t) => {
    await t.cli("index");
    await t.cli("query references a.c:2:22 --include-declaration");
});
