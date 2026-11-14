import { test } from "@clice/tools/testing";

// `clice command` shows the settings the rules give a file, each with the
// rule deciding it: the first matching rule that sets it.
test(async (t) => {
    await t.cli("command third_party/lib.cpp");
    await t.cli("command src/main.cpp --json");
});
