/// `clice index` on a workspace a server indexed before.

import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

/// The server settled with `name` among its workspace symbols.
async function indexedSymbol(s: Serve, name: string): Promise<boolean> {
    await s.indexed();
    return ((await s.workspaceSymbols(name)) ?? []).some((symbol) => symbol.name === name);
}

serve.files({
    // The cache where `clice index` looks for it.
    "clice.toml": '[project]\ncache_dir = "${workspace}/.clice"\n',
    "a.h": "#pragma once\ninline int alpha() { return 1; }\n",
    "main.cpp": '#include "a.h"\nint app_entry() { return alpha(); }\n',
})("index reports header losing host", async ({ s }) => {
    // Standalone-index a.h: edit it on disk while its buffer is open, so the
    // close sees the shard/disk mismatch and reindexes the header with
    // main.cpp as its borrowed host. `beta` can only come from that reindex —
    // the tracker loops are off and main.cpp is never touched again.
    expect(await indexedSymbol(s, "alpha"), "TU never indexed").toBe(true);
    await s.compiled("a.h");
    s.disk.edit("a.h", {
        after: "inline int alpha() { return 1; }\n",
        insert: "inline int beta() { return 2; }\n",
    });
    s.close("a.h");
    expect(await indexedSymbol(s, "beta"), "header never standalone-indexed").toBe(true);
    await s.stop();

    // Offline, the host's command changes and its include of a.h vanishes:
    // reconciliation drops the header's index and no TU can host it any more,
    // so the batch run must report the header as lost coverage.
    s.disk.write("main.cpp", "int app_entry() { return 0; }\n");
    s.disk.database({ ...s.manifest, units: { "main.cpp": ["-DHOST_V2"] } });

    const second = await s.cli("index", "--workers", "2");
    expect(second.status, `stderr: ${second.stderr}`).toBe(1);
    expect(second.stderr).toContain("stays uncovered");
    expect(second.stdout).toContain("failed to index");

    // The debt persists across runs: the snapshot keeps recording the
    // dropped header, so every rerun retries it and reports the partial
    // index rather than going silently clean.
    const third = await s.cli("index", "--workers", "2");
    expect(third.status, `stderr: ${third.stderr}`).toBe(1);
    expect(third.stderr).toContain("stays uncovered");

    // Only deleting the file settles the debt.
    s.disk.rm("a.h");
    const fourth = await s.cli("index", "--workers", "2");
    expect(fourth.status, `stderr: ${fourth.stderr}`).toBe(0);
});
