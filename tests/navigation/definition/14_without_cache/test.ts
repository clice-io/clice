import { test } from "@clice/tools/testing";

// No cache directory, as for a file opened with no workspace folder or in a
// folder clice cannot write: no PCH holds the header, and no background
// index reaches it, so the open file's compile is all the index there is.
test(async (t) => {
    const s = await t.start({
        cacheDir: "main.cpp",
        config: { logging_dir: "${workspace}/logs" },
        lastRule: { background_index: false },
    });
    s.open("main.cpp");
    await s.errors("main.cpp");
    await t.show("definition", s.definition(t.at("main.cpp", "return |helper()")));
});
