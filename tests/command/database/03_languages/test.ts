import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Cannot find driver";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// A CUDA header is C-family code though clang's table of suffixes does
// not list .cuh, and so is a file of any suffix the command's -x names a
// C-family language for; the entries of other languages a build lists
// (a resource script, an .ixx without -x) stay as written. The missing
// nvcc's entry names a CUDA installation no machine has, so clang's own
// driver, reading it, finds none on any.
test({ errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
