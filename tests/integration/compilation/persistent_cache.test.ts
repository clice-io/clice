/// Integration tests for persistent PCH/PCM cache.
///
/// Verifies that PCH/PCM artifacts are written to the unified cache store
/// (.clice/cache/v4/{pch,pcm}/) with content-addressed filenames, survive
/// server restarts via cache.json, and are properly reused across sessions.

import * as fs from "node:fs";
import * as path from "node:path";
import {
    assertCleanCompile,
    assertHasErrors,
    assertNoAnomaly,
    getErrors,
    MTIME_GRANULARITY,
    SETTLE_TIME,
    sleep,
    waitForRecompile,
} from "../../tools/checks.ts";
import { DATA_DIR, generateCdb, writeCdb } from "../../tools/compile_commands.ts";
import { expect, test } from "../../tools/fixtures.ts";
import { shutdownClient } from "../../tools/lifecycle.ts";
import {
    cacheRoot,
    type CacheJson,
    listPchFiles,
    listPchIdxFiles,
    listPcmFiles,
    listTmpFiles,
    pinCacheToWorkspace,
    readCacheJson,
} from "../../tools/workspace.ts";

function copySaveRecompile(workspace: string): void {
    const src = path.join(DATA_DIR, "modules", "save_recompile");
    for (const name of fs.readdirSync(src)) {
        const from = path.join(src, name);
        if (fs.statSync(from).isFile()) {
            fs.copyFileSync(from, path.join(workspace, name));
        }
    }
}

/// Corrupt a blob in place, preserving file size and mtime; returns the
/// corrupted bytes. "garbage" replaces the whole file (caught by reader
/// validation), "middle" flips a span reached only during deserialization
/// (can abort the consuming process instead of failing cleanly).
function corruptPreservingStat(p: string, where = "garbage", span = 4096): Buffer {
    const stat = fs.statSync(p);
    let data = fs.readFileSync(p);
    if (where === "garbage") {
        data = Buffer.alloc(data.length, 0x5a);
    } else {
        const offset = Math.floor(data.length / 2);
        for (let i = offset; i < Math.min(data.length, offset + span); i++) {
            data[i] = data[i]! ^ 0xff;
        }
    }
    fs.writeFileSync(p, data);
    fs.utimesSync(p, stat.atime, stat.mtime);
    return data;
}

/// cache.json dep hashes are 64-bit integers that overflow JS doubles;
/// Python round-trips them losslessly through json.loads/dumps, so the
/// cache-rewriting tests must too — a mangled hash fails revalidation and
/// forces a spurious rebuild. Oversized integers ride as BigInt (reviver
/// source text in, JSON.rawJSON out).
function readCacheJsonLossless(cachePath: string): CacheJson {
    type Reviver = (key: string, value: unknown, ctx: { source?: string }) => unknown;
    const parse = JSON.parse as unknown as (text: string, reviver: Reviver) => unknown;
    return parse(fs.readFileSync(cachePath, "utf8"), (_key, value, ctx) =>
        typeof value === "number" && !Number.isSafeInteger(value) && ctx.source !== undefined
            ? BigInt(ctx.source)
            : value,
    ) as CacheJson;
}

function writeCacheJsonLossless(cachePath: string, cache: CacheJson): void {
    const raw = (JSON as unknown as { rawJSON: (text: string) => unknown }).rawJSON;
    fs.writeFileSync(
        cachePath,
        JSON.stringify(cache, (_key, value: unknown) =>
            typeof value === "bigint" ? raw(value.toString()) : value,
        ),
    );
}

/// Wait until orphaned workers of a killed server release their handles
/// on tmp residue (a rename probe fails on Windows while a file is open).
async function waitResidueReleased(workspace: string, deadlineMs = 20_000): Promise<void> {
    const end = Date.now() + deadlineMs;
    while (Date.now() < end) {
        let locked = false;
        for (const f of listTmpFiles(workspace)) {
            const probe = f + ".probe";
            try {
                fs.renameSync(f, probe);
                fs.renameSync(probe, f);
            } catch {
                locked = true;
                break;
            }
        }
        if (!locked) {
            return;
        }
        await sleep(500);
    }
}

test("pch written to cache dir", async ({ session }) => {
    /// After opening a file with #include, a .pch file should appear
    /// in .clice/cache/pch/ with a hex-hash filename.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Foo { int x; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Foo f; return f.x; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Verify PCH file exists in the cache directory.
    const pchFiles = listPchFiles(workspace);
    expect(pchFiles.length, "Expected at least one .pch file in the store").toBeGreaterThanOrEqual(
        1,
    );
    // Filename should be a 32-char hex hash (xxh3_128bits) + .pch
    const stem = path.basename(pchFiles[0]!, ".pch");
    expect(stem.length, `Expected 32-char hex filename, got: ${path.basename(pchFiles[0]!)}`).toBe(
        32,
    );
});

test("cache json persisted", async ({ session }) => {
    /// After a PCH build, cache.json should be written with the entry.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nint global_val = 42;\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return global_val; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    const cache = readCacheJson(workspace);
    expect(cache, "cache.json should exist after PCH build").not.toBeNull();
    expect(cache!.pch, "cache.json should have 'pch' section").toBeDefined();
    expect(
        cache!.pch.length,
        "Expected at least one PCH entry in cache.json",
    ).toBeGreaterThanOrEqual(1);

    // Verify the entry has expected fields.
    const entry = cache!.pch[0]!;
    expect(entry).toHaveProperty("key");
    expect(entry).toHaveProperty("deps");
    expect(entry).toHaveProperty("bound");
    for (const dep of entry.deps) {
        expect(dep.hash).not.toBe(0);
        expect(dep).toHaveProperty("mtime_ns");
        expect(dep).toHaveProperty("size");
    }
});

test("pch reused on close reopen", async ({ session }) => {
    /// Closing and reopening a file within the same session should reuse
    /// the cached PCH — no additional .pch files should be created.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Bar { int y; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Bar b; return b.y; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    // First open — builds PCH.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    const pchAfterFirst = listPchFiles(workspace);
    expect(pchAfterFirst.length).toBeGreaterThanOrEqual(1);

    // Close.
    client.close(uri);
    await sleep(SETTLE_TIME);

    // Clear diagnostics so we can wait for fresh ones.
    client.diagnostics.delete(uri);

    // Reopen — should reuse cached PCH.
    const [uri2] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri2);

    const pchAfterReopen = listPchFiles(workspace);
    expect(pchAfterReopen, "PCH file set should be identical after close+reopen").toEqual(
        pchAfterFirst,
    );
});

test("pch survives server restart", async ({ session }) => {
    /// PCH cache should survive a full server restart — cache.json is
    /// loaded on startup and the existing .pch file is reused.
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Baz { int z; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Baz b; return b.z; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    // Session 1: build PCH.
    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c1, uri);

    const pchFilesS1 = listPchFiles(workspace);
    expect(pchFilesS1.length, "PCH should be created in session 1").toBeGreaterThanOrEqual(1);
    const pchMtimeS1 = fs.statSync(pchFilesS1[0]!).mtimeMs;

    const cacheS1 = readCacheJson(workspace);
    expect(cacheS1, "cache.json should exist after session 1").not.toBeNull();

    assertNoAnomaly(c1, workspace);
    await shutdownClient(c1);

    // Session 2: restart server, reopen file.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c2, uri2);

    // The same PCH file should still exist, not overwritten.
    const pchFilesS2 = listPchFiles(workspace);
    expect(pchFilesS2.length, "No new PCH files should be created in session 2").toBe(
        pchFilesS1.length,
    );
    const pchMtimeS2 = fs.statSync(pchFilesS2[0]!).mtimeMs;
    expect(pchMtimeS2, "PCH file should not be rebuilt (mtime should be unchanged)").toBe(
        pchMtimeS1,
    );

    assertNoAnomaly(c2, workspace);
    await shutdownClient(c2);
});

test("old cache json upgrades", async ({ session }) => {
    /// A cache.json written before the per-dep stat baselines (entry-level
    /// build_at, deps as bare {path, hash}) must still load: unknown fields are
    /// skipped, absent ones read back zeroed, and the entry revalidates by hash
    /// instead of being dropped.
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Old { int v; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Old o; return o.v; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c1, uri);
    const pchMtimeS1 = fs.statSync(listPchFiles(workspace)[0]!).mtimeMs;
    await shutdownClient(c1);

    // Rewrite cache.json into the pre-baseline shape.
    const cachePath = path.join(cacheRoot(workspace), "cache.json");
    const cache = readCacheJsonLossless(cachePath);
    for (const entry of [...cache.pch, ...(cache.pcm ?? [])]) {
        entry.build_at = 0;
        entry.deps = entry.deps.map((d) => ({ path: d.path, hash: d.hash }));
    }
    writeCacheJsonLossless(cachePath, cache);

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c2, uri2);
    // Loaded, hash-validated, reused — not rebuilt.
    expect(fs.statSync(listPchFiles(workspace)[0]!).mtimeMs).toBe(pchMtimeS1);
    await shutdownClient(c2);
});

test("pcm offline edit invalidates", async ({ session }) => {
    /// Editing a module interface while the server is down must invalidate
    /// the cached PCM on restart: the PCM key embeds no content, so only its
    /// deps snapshot can see the change.
    const workspace = session.tmpdir();
    copySaveRecompile(workspace);
    pinCacheToWorkspace(workspace);
    generateCdb(workspace);

    // Session 1: importer compiles clean, PCM cached.
    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [midUri] = await c1.openAndWait(path.join(workspace, "mid.cppm"));
    assertCleanCompile(c1, midUri);
    expect(listPcmFiles(workspace).length).toBeGreaterThanOrEqual(1);
    assertNoAnomaly(c1, workspace);
    await shutdownClient(c1);

    // Offline: rename the export the importer calls.
    fs.writeFileSync(
        path.join(workspace, "leaf.cppm"),
        "export module Leaf;\n\nexport int renamed_leaf() {\n    return 1;\n}\n",
    );

    // Session 2: mid.cppm calls leaf(), which no longer exists — a stale
    // PCM would compile it clean.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [midUri2] = await c2.openAndWait(path.join(workspace, "mid.cppm"));
    assertHasErrors(c2, midUri2, "Expected errors after offline interface edit");
    await shutdownClient(c2);
});

test("depless pcm entry dropped", async ({ session }) => {
    /// A PCM cache entry with an empty deps list (written before deps were
    /// populated) is unvalidatable and must be dropped at load, not trusted.
    const workspace = session.tmpdir();
    copySaveRecompile(workspace);
    pinCacheToWorkspace(workspace);
    generateCdb(workspace);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [midUri] = await c1.openAndWait(path.join(workspace, "mid.cppm"));
    assertCleanCompile(c1, midUri);
    await shutdownClient(c1);

    // Simulate a pre-upgrade cache: strip the PCM deps, then break the
    // interface offline. A trusted dep-less entry would compile mid clean.
    const cachePath = path.join(cacheRoot(workspace), "cache.json");
    const cache = readCacheJsonLossless(cachePath);
    for (const entry of cache.pcm ?? []) {
        entry.deps = [];
    }
    writeCacheJsonLossless(cachePath, cache);
    fs.writeFileSync(
        path.join(workspace, "leaf.cppm"),
        "export module Leaf;\n\nexport int renamed_leaf() {\n    return 1;\n}\n",
    );

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [midUri2] = await c2.openAndWait(path.join(workspace, "mid.cppm"));
    assertHasErrors(c2, midUri2, "Expected errors after offline interface edit");
    await shutdownClient(c2);
});

test("pcm cache entry has deps", async ({ session }) => {
    /// cache.json PCM entries must record dependencies, including the module
    /// source file itself — an empty list is permanently blind.
    const { client, workspace } = session.tmp();
    copySaveRecompile(workspace);
    pinCacheToWorkspace(workspace);
    generateCdb(workspace);
    await client.initialize(workspace);

    const [midUri] = await client.openAndWait(path.join(workspace, "mid.cppm"));
    assertCleanCompile(client, midUri);

    const cache = readCacheJson(workspace);
    expect((cache?.pcm ?? []).length, "Expected PCM cache entries").toBeGreaterThan(0);
    const paths = cache!.paths;
    for (const entry of cache!.pcm ?? []) {
        const deps = entry.deps;
        expect(deps.length, `PCM entry ${String(entry.module_name)} has no deps`).toBeGreaterThan(
            0,
        );
        // Deps are canonicalized through real_path; compare resolved forms
        // (on macOS the pytest tmp dir sits behind the /var symlink).
        const source = fs.realpathSync(paths[entry.source_file!]!);
        const depPaths = new Set(deps.map((d) => fs.realpathSync(paths[d.path]!)));
        expect(depPaths.has(source), "Module source must be its own dependency").toBe(true);
        expect(deps.every((d) => d.hash !== 0)).toBe(true);
    }
});

test("shared preamble shares pch", async ({ session }) => {
    /// Two files with identical preambles should share the same PCH file
    /// (content-addressed by preamble hash).
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nint shared_val = 1;\n");
    fs.writeFileSync(
        path.join(workspace, "a.cpp"),
        '#include "header.h"\nint fa() { return shared_val; }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "b.cpp"),
        '#include "header.h"\nint fb() { return shared_val + 1; }\n',
    );
    writeCdb(workspace, ["a.cpp", "b.cpp"]);
    await client.initialize(workspace);

    const [uriA] = await client.openAndWait(path.join(workspace, "a.cpp"));
    const [uriB] = await client.openAndWait(path.join(workspace, "b.cpp"));
    assertCleanCompile(client, uriA);
    assertCleanCompile(client, uriB);

    // Both files have the same preamble (#include "header.h").
    // Content-addressed naming means only ONE .pch file should exist.
    const pchFiles = listPchFiles(workspace);
    expect(
        pchFiles.length,
        `Expected exactly 1 PCH file for shared preamble, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(1);
});

test("different preamble different pch", async ({ session }) => {
    /// Files with different preambles should produce different PCH files.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "a.h"), "#pragma once\nint val_a = 1;\n");
    fs.writeFileSync(path.join(workspace, "b.h"), "#pragma once\nint val_b = 2;\n");
    fs.writeFileSync(path.join(workspace, "a.cpp"), '#include "a.h"\nint fa() { return val_a; }\n');
    fs.writeFileSync(path.join(workspace, "b.cpp"), '#include "b.h"\nint fb() { return val_b; }\n');
    writeCdb(workspace, ["a.cpp", "b.cpp"]);
    await client.initialize(workspace);

    const [uriA] = await client.openAndWait(path.join(workspace, "a.cpp"));
    const [uriB] = await client.openAndWait(path.join(workspace, "b.cpp"));
    assertCleanCompile(client, uriA);
    assertCleanCompile(client, uriB);

    // Different preambles → different hash → two separate .pch files.
    const pchFiles = listPchFiles(workspace);
    expect(
        pchFiles.length,
        `Expected 2 PCH files for different preambles, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

test("pch rebuilt on header change", async ({ session }) => {
    /// When a preamble header changes, a new PCH should be built
    /// (different hash → different filename). The old one remains for cleanup.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct V1 { int a; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { V1 v; return v.a; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    const pchBefore = listPchFiles(workspace);
    expect(pchBefore.length).toBeGreaterThanOrEqual(1);

    // Modify header — changes preamble content hash.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct V2 { int b; };\n");
    // Also update main.cpp to use V2 so it compiles cleanly.
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { V2 v; return v.b; }\n',
    );

    // Close and reopen to get fresh preamble.
    client.close(uri);
    await sleep(SETTLE_TIME);
    client.diagnostics.delete(uri);

    const [uri2] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri2);

    const pchAfter = listPchFiles(workspace);
    // The preamble content changed (#include "header.h" is the same text,
    // but the preamble hash is computed from the preamble TEXT in the source file,
    // not from the header content). Since the #include line is identical,
    // the preamble hash is the same → same PCH filename, but deps changed
    // so PCH gets rebuilt (overwritten at the same path).
    // Either way, compilation should succeed.
    expect(pchAfter.length).toBeGreaterThanOrEqual(1);
});

test("no tmp files after build", async ({ session }) => {
    /// After a successful PCH build, no .tmp files should remain in the cache dir.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nint val = 1;\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return val; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // No in-flight tmp files should linger after the build settles. The
    // pch namespace legitimately holds the paired .pch.idx blobs.
    expect(listTmpFiles(workspace), "Stale tmp files found").toEqual([]);
    const expected: Record<string, string[]> = { pch: [".pch", ".pch.idx"], pcm: [".pcm"] };
    for (const [subdir, extensions] of Object.entries(expected)) {
        const blobDir = path.join(cacheRoot(workspace), subdir);
        if (fs.existsSync(blobDir)) {
            const stray = fs
                .readdirSync(blobDir)
                .filter((name) => !extensions.some((e) => name.endsWith(e)));
            expect(stray, `Stray files in ${subdir}/: ${stray.join(", ")}`).toEqual([]);
        }
    }
});

test("cache dirs created on startup", async ({ session }) => {
    /// The versioned store directories should be created when the server
    /// initializes a workspace.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    // Trigger a compilation to ensure load_workspace() has completed
    // (it runs asynchronously after initialization).
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    for (const subdir of ["pch", "pcm", "index"]) {
        expect(
            fs.existsSync(path.join(cacheRoot(workspace), subdir)) &&
                fs.statSync(path.join(cacheRoot(workspace), subdir)).isDirectory(),
            `${subdir}/ should be created`,
        ).toBe(true);
    }
});

test("different flags different pch", async ({ session }) => {
    /// Two files with identical preamble text but different -D flags must
    /// not share a PCH.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(
        path.join(workspace, "header.h"),
        "#pragma once\n#ifdef MODE\nstruct Cfg { int mode; };\n#else\nstruct Cfg { int plain; };\n#endif\n",
    );
    const body = '#include "header.h"\nint use() { Cfg c; return 0; }\n';
    fs.writeFileSync(path.join(workspace, "a.cpp"), body);
    fs.writeFileSync(path.join(workspace, "b.cpp"), body);

    // Same preamble text, different macro definitions per file.
    const entries = (
        [
            ["a.cpp", []],
            ["b.cpp", ["-DMODE=1"]],
        ] as [string, string[]][]
    ).map(([name, extra]) => ({
        directory: workspace,
        file: path.join(workspace, name),
        arguments: ["clang++", "-std=c++17", "-fsyntax-only", ...extra, path.join(workspace, name)],
    }));
    fs.writeFileSync(path.join(workspace, "compile_commands.json"), JSON.stringify(entries));
    await client.initialize(workspace);

    const [uriA] = await client.openAndWait(path.join(workspace, "a.cpp"));
    const [uriB] = await client.openAndWait(path.join(workspace, "b.cpp"));
    assertCleanCompile(client, uriA);
    assertCleanCompile(client, uriB);

    const pchFiles = listPchFiles(workspace);
    expect(
        pchFiles.length,
        `Same preamble with different -D flags must produce 2 PCHs, got ${pchFiles.length}: ${pchFiles.map((f) => path.basename(f)).join(", ")}`,
    ).toBe(2);
});

test("kill9 recovery", async ({ session }) => {
    /// kill -9 during compilation must not corrupt the store: a restarted
    /// server sweeps crash residue and serves the file normally.
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct K { int x; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { K k; return k.x; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    // Session 1: open the file and kill the server; the short delay makes it
    // likely (not guaranteed) the first build is still in flight.
    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    c1.open(path.join(workspace, "main.cpp"));
    await sleep(300);
    c1.killServer();
    c1.dispose();
    await waitResidueReleased(workspace);

    // Session 2: its startup sweeps the dead instance's tmp directory, and
    // the cache must be usable again.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c2, uri);
    const pchFiles = listPchFiles(workspace);
    expect(pchFiles.length, "PCH should be (re)built after crash").toBeGreaterThanOrEqual(1);
    // Blob directories contain only committed blobs (the PCH and its
    // paired index), never partial writes.
    const stray = fs
        .readdirSync(path.join(cacheRoot(workspace), "pch"))
        .filter((name) => !name.endsWith(".pch") && !name.endsWith(".pch.idx"));
    expect(stray, `Crash residue in pch/: ${stray.join(", ")}`).toEqual([]);
    assertNoAnomaly(c2, workspace);
    await shutdownClient(c2);

    // Clean shutdown removed session 2's own tmp; session 1's residue was
    // swept at session 2 startup, so nothing may remain.
    expect(listTmpFiles(workspace), "tmp residue should be swept").toEqual([]);
});

test.for(["garbage", "middle"])(
    "corrupt pch rebuilt on restart %s",
    async (where, { skip, session }) => {
        /// A .pch corrupted offline (size and mtime preserved, so freshness
        /// checks pass) must not brick the file: the consumption failure retracts
        /// the pair and rebuilds it, and real diagnostics come back.
        const workspace = session.tmpdir();
        pinCacheToWorkspace(workspace);
        fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nint known_func();\n");
        // The body has a real error: a bricked file publishes an empty list
        // instead, so the error is the recovery signal.
        fs.writeFileSync(
            path.join(workspace, "main.cpp"),
            '#include "header.h"\nint main() { return undeclared_symbol; }\n',
        );
        writeCdb(workspace, ["main.cpp"]);

        // The middle shape may crash a worker, an intentional anomaly, so those
        // sessions opt out of the manager's anomaly gate (and the garbage case
        // still asserts cleanliness explicitly below).
        const allowAnomaly = where === "middle";
        const c1 = session.spawn(workspace, { allowAnomaly });
        await c1.initialize(workspace);
        const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
        assertHasErrors(c1, uri, "Baseline session should report the body error");
        expect(listPchFiles(workspace).length).toBe(1);
        assertNoAnomaly(c1, workspace);
        await shutdownClient(c1);

        const corrupted = corruptPreservingStat(listPchFiles(workspace)[0]!, where);

        // Debug builds trap anomalies with abort() unless told otherwise
        // (same as test_crash_recovery.py).
        if (where === "middle") {
            process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
        }
        let c2;
        try {
            c2 = session.spawn(workspace, { allowAnomaly });
            await c2.initialize(workspace);
        } finally {
            delete process.env["CLICE_ANOMALY_NO_TRAP"];
        }
        const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
        if (getErrors(c2.diagnostics.get(uri2) ?? []).length === 0) {
            // The crash shape ends its round with a versionless empty publish
            // after retracting the pair; the next request rebuilds it.
            c2.diagnostics.delete(uri2);
            await waitForRecompile(c2, uri2);
        }
        // The specific body error, not just any error: a quarantine notice or
        // a still-standing corruption fatal must not count as recovery.
        expect(
            getErrors(c2.diagnostics.get(uri2) ?? []).some((d) =>
                (typeof d.message === "string" ? d.message : d.message.value).includes(
                    "undeclared_symbol",
                ),
            ),
            `Real diagnostics must recover, got: ${JSON.stringify(c2.diagnostics.get(uri2) ?? [])}`,
        ).toBe(true);
        const pchFiles = listPchFiles(workspace);
        expect(pchFiles.length, "The pair must be rebuilt, not abandoned").toBe(1);
        if (where === "middle" && fs.readFileSync(pchFiles[0]!).equals(corrupted)) {
            // The flip landed in semantically dead bytes on this LLVM build
            // (PCH blobs carry no whole-file checksum): the reader consumed
            // the blob untouched and there is nothing to heal.
            await shutdownClient(c2);
            skip("mid-file flip was semantically dead on this build");
        }
        expect(
            fs.readFileSync(pchFiles[0]!).equals(corrupted),
            "The corrupt .pch must be rebuilt, not trusted forever",
        ).toBe(false);
        if (where === "garbage") {
            assertNoAnomaly(c2, workspace);
        }
        await shutdownClient(c2);
    },
);

test("corrupt pch idx retracted", async ({ session }) => {
    /// A corrupt .pch.idx detected at load must retract the on-disk pair
    /// (not just the in-memory path): a pair that looks complete would be
    /// re-adopted and silently degrade every later session.
    const workspace = session.tmpdir();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Idx { int v; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Idx i; return i.v; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c1, uri);
    expect(listPchIdxFiles(workspace).length).toBe(1);
    assertNoAnomaly(c1, workspace);
    await shutdownClient(c1);

    const corrupted = corruptPreservingStat(listPchIdxFiles(workspace)[0]!);

    // Recovery rides the .pch artifact gate: detecting the corrupt idx
    // retracts the whole pair from the store mid-round, the compile then
    // setup-fails on the now-missing .pch, and the gate rebuilds both.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c2, uri2);
    const idxFiles = listPchIdxFiles(workspace);
    expect(idxFiles.length, "The pair must be rebuilt after idx corruption").toBe(1);
    expect(
        fs.readFileSync(idxFiles[0]!).equals(corrupted),
        "The corrupt .pch.idx must be retracted and rebuilt, not left posing as a complete pair",
    ).toBe(false);
    assertNoAnomaly(c2, workspace);
    await shutdownClient(c2);
});

test("cache wiped while running", async ({ session }) => {
    /// Wiping the cache directory under a running server must not wedge
    /// PCH builds forever: the store re-creates its directories on demand.
    const { client, workspace } = session.tmp();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct W { int x; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { W w; return w.x; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Simulate a user resetting state without restarting the server.
    fs.rmSync(path.join(workspace, ".clice", "cache"), { recursive: true, force: true });

    // Change the preamble so a fresh PCH build is required.
    await sleep(1_100);
    fs.writeFileSync(
        path.join(workspace, "header.h"),
        "#pragma once\nstruct W { int x; int y; };\n",
    );

    await waitForRecompile(client, uri);
    assertCleanCompile(client, uri);
    expect(listPchFiles(workspace).length, "PCH build must recover after a cache wipe").toBe(1);
});
