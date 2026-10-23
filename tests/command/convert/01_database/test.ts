import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";
const UNCONVERTED_ENTRIES = "of the entries stay driver commands";

// Each entry becomes the command clice compiles its file with; the
// assembler source, which clice does not compile, stays as written. The
// compiler does not exist, so the run fails after printing the database.
test("each entry", { errors: [MISSING, UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert compile_commands.json");
});

// A relative directory is the database's own, written out absolute.
test("a relative directory", { errors: ["Cannot find driver", UNCONVERTED_ENTRIES] }, async (t) => {
    await t.cli("command convert build/compile_commands.json");
});

test("not a database", { errors: ["holds no array of entries"] }, async (t) => {
    await t.cli("command convert broken.json");
});
