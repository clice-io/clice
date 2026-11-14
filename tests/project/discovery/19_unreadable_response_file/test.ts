import { test } from "@clice/tools/testing";

// The database names a response file that does not exist: the compiler
// would stop on it, and clice, which leaves it out of the command, says so.
test(async (t) => {
    await t.cli("index");
    await t.cli("query compile-command main.cpp");
});
