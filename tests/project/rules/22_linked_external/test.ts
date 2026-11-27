import * as path from "node:path";
import { test } from "@clice/tools/testing";

// Bazel links an external repository from its cache: the build reaches a
// file through external/, its real path lies elsewhere, and a pattern
// matching either path applies.
test(async (t) => {
    t.disk.linkDirectory("external/kotatsu", "cache/kotatsu");
    const entry = {
        directory: t.root,
        file: "external/kotatsu/a.cpp",
        arguments: [path.join(t.root, "missing-cc"), "-c", "external/kotatsu/a.cpp"],
    };
    t.disk.write("compile_commands.json", JSON.stringify([entry], null, 2));
    await t.cli("command external/kotatsu/a.cpp");
});
