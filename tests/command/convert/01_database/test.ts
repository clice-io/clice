import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";
const UNRESOLVED = "2 of the converted entries could not be resolved";

// Each entry becomes the command clice compiles its file with; the
// assembler source, which clice does not compile, stays as written. The
// compiler does not exist, so the run fails after printing the database.
test({ errors: [MISSING, UNRESOLVED] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});
