import { test } from "@clice/tools/testing";

// A header has no entry of its own: the command says so rather than show
// the command of a source including it.
test({ errors: ["No compilation database lists", "no such file"] }, async (t) => {
    await t.cli("command util.h");
    await t.cli("command missing.cpp");
});
