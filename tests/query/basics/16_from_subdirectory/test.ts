import { test } from "@clice/tools/testing";

// Asked from a subdirectory without --workspace, a command works on the
// project above it, as git does, and takes paths relative to where it is
// asked.
test(async (t) => {
    await t.cli("index");
    t.disk.write("build/.keep", "");
    await t.cli("query project-files", { cwd: "build" });
    await t.cli("query file-deps ../main.cpp", { cwd: "build" });
    await t.cli("query definition ../main.cpp:3", { cwd: "build" });
});
