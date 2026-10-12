import { test } from "@clice/tools/testing";

// Asked from a build directory with the workspace spelled `..`, answers
// still name the workspace's files.
test(async (t) => {
    await t.cli("index");
    t.disk.write("build/.keep", "");
    await t.cli("query --workspace .. --method definition --symbol Animal", { cwd: "build" });
    await t.cli("query --workspace .. --method projectFiles", { cwd: "build" });
});
