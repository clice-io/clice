import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNRESOLVED = "of the converted entries could not be resolved";

// A CUDA header is C-family code though clang's table of suffixes does
// not list .cuh, and so is a file of any suffix the command's -x names a
// C-family language for; the entries of other languages a build lists
// (a resource script, an .ixx without -x) stay as written.
test({ errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
