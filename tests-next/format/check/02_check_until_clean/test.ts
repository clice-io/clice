import { test } from "@clice/tools/testing";

// --check lists every file to format and rewrites nothing; once they are
// formatted it passes. The vendored and the system header are no file of
// the build's own, checked or not.
test(async (t) => {
    await t.cli("format --check");
    await t.cli("format");
    await t.cli("format --check");
});
