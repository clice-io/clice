/// Several workspace folders served by one server: each folder is a project
/// with its own compilation database and cache, files are routed to the
/// project that compiles them, and folders come and go at runtime.

import * as fs from "node:fs";
import { runProcess } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

/// How long background indexing of these few-file folders may take.
const INDEX_TIMEOUT = 30_000;

test("shared cache directory serves one project", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.write("a/main.cpp", "int in_a() { return 0; }\n");
    workspace.write("b/main.cpp", "int in_b() { return 0; }\n");
    workspace.writeCDB(["a/main.cpp"], { at: "a/compile_commands.json" });
    workspace.writeCDB(["b/main.cpp"], { at: "b/compile_commands.json" });
    const shared = `[project]\ncache_dir = '${workspace.path("shared")}'\n`;
    workspace.write("a/clice.toml", shared);
    workspace.write("b/clice.toml", shared);
    const index = (folder: string) =>
        runProcess(
            cliceExecutable(),
            ["index", "--workspace", workspace.path(folder), "--workers", "1"],
            { timeout: INDEX_TIMEOUT },
        );

    expect((await index("a")).status).toBe(0);
    // Run after it, the other project indexes into a cache of its own
    // instead of the first one's.
    const second = await index("b");
    expect(second.status, `stderr: ${second.stderr}`).toBe(0);
    expect(second.stdout).toContain("Indexed 1 translation unit");
    expect(fs.existsSync(workspace.path("b/.clice"))).toBe(true);
});
