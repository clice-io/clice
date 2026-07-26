/// Integration tests for the PCH overlay: header symbols of open in-memory
/// files resolve through the PCH's paired index blob, independent of the disk
/// index and faithful to the live buffer's preprocessor context.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import {
    locationsOf,
    MTIME_GRANULARITY,
    sleep,
    waitForIndex,
    waitForRecompile,
} from "../../tools/checks.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { test, expect } from "../../tools/fixtures.ts";
import { shutdownClient } from "../../tools/lifecycle.ts";

const NO_INDEXING = { project: { enable_indexing: false } };

/// Narrow a definition response (which may be typed as LocationLink[]) to
/// plain Locations; the server always returns Locations.
function asLocations(result: unknown): proto.Location[] {
    return locationsOf(result as proto.Location | proto.Location[] | null);
}

// Versioned root of the unified cache store; bump together with
// cache_format_version in src/server/state/workspace.h.
const CACHE_ROOT = path.join(".clice", "cache", "v4");

function cacheRoot(workspace: string): string {
    return path.join(workspace, CACHE_ROOT);
}

function listPchFiles(workspace: string): string[] {
    const dir = path.join(cacheRoot(workspace), "pch");
    if (!fs.existsSync(dir)) {
        return [];
    }
    return fs
        .readdirSync(dir)
        .filter((name) => name.endsWith(".pch"))
        .sort()
        .map((name) => path.join(dir, name));
}

function listPchIdxFiles(workspace: string): string[] {
    const dir = path.join(cacheRoot(workspace), "pch");
    if (!fs.existsSync(dir)) {
        return [];
    }
    return fs
        .readdirSync(dir)
        .filter((name) => name.endsWith(".pch.idx"))
        .sort()
        .map((name) => path.join(dir, name));
}

/// Write a clice.toml that pins cache_dir to <workspace>/.clice/.
function pinCacheToWorkspace(workspace: string): void {
    fs.writeFileSync(
        path.join(workspace, "clice.toml"),
        '[project]\ncache_dir = "${workspace}/.clice"\n',
    );
}

test("definition into unindexed header", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "foo.h"), "inline void foo() {}\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    // With background indexing off, only the PCH overlay knows the header.
    const locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h") && loc.range.start.line === 0)).toBe(true);
});

test("references include header rows", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "foo.h"),
        "inline void foo() {}\ninline void bar() { foo(); }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    const refs = locationsOf(await client.referencesAt(uri, 1, 13));
    expect(refs.some((r) => r.uri.endsWith("foo.h") && r.range.start.line === 1)).toBe(true);
    expect(refs.some((r) => r.uri.endsWith("main.cpp"))).toBe(true);
});

test("buffer context overrides disk", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "crypto.h"),
        "#ifdef USE_A\ninline void only_a() {}\n#else\ninline void only_b() {}\n#endif\n",
    );
    const diskText = '#include "crypto.h"\nint main() { only_b(); return 0; }\n';
    fs.writeFileSync(path.join(workspace, "main.cpp"), diskText);
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(await waitForIndex(client, uri, "only_b"), "Index not ready after 30s").toBe(true);

    // The buffer's preamble now activates the branch no disk context has
    // ever seen; only the rebuilt PCH's overlay can resolve only_a.
    client.change(
        uri,
        2,
        '#define USE_A 1\n#include "crypto.h"\nint main() { only_a(); return 0; }\n',
    );
    await waitForRecompile(client, uri);

    const locs = asLocations(await client.definitionAt(uri, 2, 13));
    expect(locs.some((loc) => loc.uri.endsWith("crypto.h") && loc.range.start.line === 1)).toBe(
        true,
    );
});

test("no duplicate reference rows", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "foo.h"),
        "inline void foo() {}\ninline void bar() { foo(); }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "other.cpp"),
        '#include "foo.h"\nint other() { return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp", "other.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    // Open files are skipped by background indexing; the closed other.cpp
    // is what carries foo.h's rows into the disk index.
    expect(await waitForIndex(client, uri, "bar"), "Index not ready after 30s").toBe(true);

    // The header's rows exist in both its disk shard and the overlay; the
    // union must collapse them.
    const refs = locationsOf(await client.referencesAt(uri, 1, 13));
    const keys = refs.map((r) => `${r.uri}:${r.range.start.line}:${r.range.start.character}`);
    expect(keys.length, `duplicate reference rows: ${JSON.stringify(keys)}`).toBe(
        new Set(keys).size,
    );
    expect(refs.some((r) => r.uri.endsWith("foo.h"))).toBe(true);
});

test("preamble macro definition", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        "#define ANSWER 42\nint main() { return ANSWER; }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    // The #define lives in the preamble region swallowed by the PCH; its
    // definition is served from the overlay's main-file entry.
    const locs = asLocations(await client.definitionAt(uri, 1, 20));
    expect(locs.some((loc) => loc.uri.endsWith("main.cpp") && loc.range.start.line === 0)).toBe(
        true,
    );
});

test("preamble links survive restart", async ({ session }) => {
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "foo.h"), "inline void foo() {}\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    const links = await c1.documentLinks(uri);
    expect((links ?? []).some((link) => (link.target ?? "").endsWith("foo.h"))).toBe(true);
    const pchMtime = fs.statSync(listPchFiles(workspace)[0]!).mtimeMs;
    await shutdownClient(c1);

    // Session 2 hits the persisted PCH pair: the preamble's links must be
    // served from the reloaded blob, not lost with the process.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    const links2 = await c2.documentLinks(uri2);
    expect(
        (links2 ?? []).some((link) => (link.target ?? "").endsWith("foo.h")),
        "preamble document links lost across restart",
    ).toBe(true);
    expect(
        fs.statSync(listPchFiles(workspace)[0]!).mtimeMs,
        "PCH was rebuilt instead of reused",
    ).toBe(pchMtime);
    await shutdownClient(c2);
});

test("missing idx rebuilds pair", async ({ session }) => {
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "foo.h"), "inline void foo() {}\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    const locs = asLocations(await c1.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h"))).toBe(true);
    const pchMtime = fs.statSync(listPchFiles(workspace)[0]!).mtimeMs;
    await shutdownClient(c1);

    // Half the pair vanishes (crash residue, external cleanup): the next
    // session must treat the PCH as a miss and rebuild both blobs.
    const idxFiles = listPchIdxFiles(workspace);
    expect(idxFiles.length, "expected a committed .pch.idx next to the PCH").toBeGreaterThan(0);
    for (const idx of idxFiles) {
        fs.rmSync(idx);
    }
    await sleep(MTIME_GRANULARITY);

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    const locs2 = asLocations(await c2.definitionAt(uri2, 1, 13));
    expect(
        locs2.some((loc) => loc.uri.endsWith("foo.h")),
        "overlay dead after losing the idx half of the pair",
    ).toBe(true);
    expect(
        fs.statSync(listPchFiles(workspace)[0]!).mtimeMs,
        "PCH pair should have been rebuilt",
    ).not.toBe(pchMtime);
    expect(
        listPchIdxFiles(workspace).length,
        "rebuilt pair is missing its idx blob",
    ).toBeGreaterThan(0);
    await shutdownClient(c2);
});

test("header edit refreshes overlay", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "foo.h"), "inline void foo() {}\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "foo.h"\nint main() { foo(); return 0; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    let locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.range.start.line === 0)).toBe(true);

    // Same preamble text, so the PCH key is unchanged; the header edit must
    // still refresh the pair (deps_changed) and the served overlay with it.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "foo.h"), "// moved\ninline void foo() {}\n");
    await waitForRecompile(client, uri);

    locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h") && loc.range.start.line === 1)).toBe(true);
});
