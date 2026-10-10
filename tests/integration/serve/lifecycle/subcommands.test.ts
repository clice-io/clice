/// `clice index` on a workspace a server indexed before.

import type { Serve } from "@clice/tools/actions";
import { expect, serve } from "../../fixtures.ts";

/// The server settled with `name` among its workspace symbols.
async function indexedSymbol(s: Serve, name: string): Promise<boolean> {
    await s.indexed();
    return ((await s.workspaceSymbols(name)) ?? []).some((symbol) => symbol.name === name);
}

serve("shapes/headers", {
    // The cache where `clice index` looks for it.
    files: { "clice.toml": '[project]\ncache_dir = "${workspace}/.clice"\n' },
})("index reports header losing host", async ({ s }) => {
    // Standalone-index the limits header, which only the registry includes:
    // edit it on disk while its buffer is open, so the close sees the
    // shard/disk mismatch and reindexes the header with the registry as its
    // borrowed host. `registry_reserve` can only come from that reindex — the
    // tracker loops are off and the registry is never touched again.
    expect(await indexedSymbol(s, "registry_limit"), "TU never indexed").toBe(true);
    await s.compiled(s.file("limits"));
    s.disk.edit(s.file("limits"), {
        after: "inline constexpr int registry_limit = 64;\n",
        insert: "inline constexpr int registry_reserve = 8;\n",
    });
    s.close(s.file("limits"));
    expect(await indexedSymbol(s, "registry_reserve"), "header never standalone-indexed").toBe(
        true,
    );
    await s.stop();

    // Offline, the host's command changes and its include of the header
    // vanishes: reconciliation drops the header's index and no TU can host
    // it any more, so the batch run must report the header as lost coverage.
    s.disk.edit(
        s.file("registry"),
        { remove: '#include "registry_limits.h"\n' },
        { replace: "< registry_limit", with: "< 64" },
    );
    s.disk.database({
        ...s.manifest,
        units: { ...s.manifest.units, [s.file("registry")]: ["-DHOST_V2"] },
    });

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
    s.disk.rm(s.file("limits"));
    const fourth = await s.cli("index", "--workers", "2");
    expect(fourth.status, `stderr: ${fourth.stderr}`).toBe(0);
});
