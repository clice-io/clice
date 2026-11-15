import { test } from "@clice/tools/testing";

// The database names a response file that does not exist: the compiler
// would stop on it, and clice, which leaves it out of the command, says so.
// CMake's module map, absent until the build's scan writes it, goes
// unmentioned: it holds only module options, which clice builds itself.
test(async (t) => {
    await t.cli("index");
    await t.cli("command main.cpp");
});
