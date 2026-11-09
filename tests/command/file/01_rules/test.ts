import { test } from "@clice/tools/testing";

// The rules edit the database's command into the one clice compiles with.
test(async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --json");
});
