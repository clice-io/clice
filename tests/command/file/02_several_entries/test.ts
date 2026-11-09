import { test } from "@clice/tools/testing";

// Every entry of the file shows, in the order the build ranks them; one
// written as a `command` is split as clice reads it.
test(async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --json");
});
