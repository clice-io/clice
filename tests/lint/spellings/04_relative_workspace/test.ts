import { test } from "@clice/tools/testing";

// Pins a relative --workspace: it resolves against the working directory,
// and the findings name their files by absolute path all the same.
test(async (t) => {
    await t.cli("lint --workspace . --workers 2");
    await t.cli("lint --workspace .. --workers 2", { cwd: "src" });
});
