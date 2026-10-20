import { test } from "@clice/tools/testing";

const MISSING = "Toolchain query failed: Driver .*missing-cc not found or not executable";

// The rules of the build configuration apply: the default one, or the one
// --configuration names.
test({ errors: [MISSING] }, async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --configuration release");
});
