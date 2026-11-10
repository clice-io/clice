import { test } from "@clice/tools/testing";

// The rules of the build configuration apply: the default one, or the one
// --configuration names.
test(async (t) => {
    await t.cli("command main.cpp");
    await t.cli("command main.cpp --configuration release");
});
