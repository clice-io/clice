import { test } from "@clice/tools/testing";

// The workspace is src/; its rule takes the database of the directory
// above, which is a copy of the build's and no build tree of its own: the
// unit it lists is still the workspace's to check.
test(async (t) => {
    await t.cli("format --check --workspace src");
});
