import { test } from "@clice/tools/testing";

// The cards of a target's precompiled header come back the same too: the
// check compiles the headers' text, where it writes each card's twin.
test(async (t) => {
    await t.cli(["inspect", "hover", "main.cpp", "--check"]);
});
