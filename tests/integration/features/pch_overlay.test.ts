/// Integration tests for the PCH overlay: header symbols of open in-memory
/// files resolve through the PCH's paired index blob, independent of the disk
/// index and faithful to the live buffer's preprocessor context.

import * as fs from "node:fs";
import { asLocations, locationsOf, MTIME_GRANULARITY, sleep } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

const NO_INDEXING = { project: { enable_indexing: false } };

test("definition into unindexed header", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "inline void foo() {}\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait("main.cpp");
    // With background indexing off, only the PCH overlay knows the header.
    const locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h") && loc.range.start.line === 0)).toBe(true);
});

test("references include header rows", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "inline void foo() {}\ninline void bar() { foo(); }\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait("main.cpp");
    const refs = locationsOf(await client.referencesAt(uri, 1, 13));
    expect(refs.some((r) => r.uri.endsWith("foo.h") && r.range.start.line === 1)).toBe(true);
    expect(refs.some((r) => r.uri.endsWith("main.cpp"))).toBe(true);
});

test("buffer context overrides disk", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "crypto.h",
        "#ifdef USE_A\ninline void only_a() {}\n#else\ninline void only_b() {}\n#endif\n",
    );
    const diskText = '#include "crypto.h"\nint main() { only_b(); return 0; }\n';
    workspace.write("main.cpp", diskText);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    expect(await client.waitForIndex(uri, "only_b"), "Index not ready after 30s").toBe(true);

    // The buffer's preamble now activates the branch no disk context has
    // ever seen; only the rebuilt PCH's overlay can resolve only_a.
    client.change(
        uri,
        2,
        '#define USE_A 1\n#include "crypto.h"\nint main() { only_a(); return 0; }\n',
    );
    await client.waitForRecompile(uri);

    const locs = asLocations(await client.definitionAt(uri, 2, 13));
    expect(locs.some((loc) => loc.uri.endsWith("crypto.h") && loc.range.start.line === 1)).toBe(
        true,
    );
});

test("no duplicate reference rows", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "inline void foo() {}\ninline void bar() { foo(); }\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.write("other.cpp", '#include "foo.h"\nint other() { return 0; }\n');
    workspace.writeCDB(["main.cpp", "other.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    // Open files are skipped by background indexing; the closed other.cpp
    // is what carries foo.h's rows into the disk index.
    expect(await client.waitForIndex(uri, "bar"), "Index not ready after 30s").toBe(true);

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
    workspace.write("main.cpp", "#define ANSWER 42\nint main() { return ANSWER; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait("main.cpp");
    // The #define lives in the preamble region swallowed by the PCH; its
    // definition is served from the overlay's main-file entry.
    const locs = asLocations(await client.definitionAt(uri, 1, 20));
    expect(locs.some((loc) => loc.uri.endsWith("main.cpp") && loc.range.start.line === 0)).toBe(
        true,
    );
});

test("preamble include hover", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "inline void foo() {}\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait("main.cpp");
    // The include lives in the preamble, invisible to the worker's AST:
    // the hover must be served from the PCH's stored links.
    const hover = await client.hoverAt(uri, 0, 12);
    expect(hover).not.toBeNull();
    expect(JSON.stringify(hover!.contents)).toContain("foo.h");
    // Link ranges are half-open: just past the closing quote hovers nothing.
    expect(await client.hoverAt(uri, 0, 16)).toBeNull();
});

test("preamble links survive restart", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    workspace.write("foo.h", "inline void foo() {}\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait("main.cpp");
    const links = await c1.documentLinks(uri);
    expect((links ?? []).some((link) => (link.target ?? "").endsWith("foo.h"))).toBe(true);
    const pchMtime = fs.statSync(workspace.pchFiles()[0]!).mtimeMs;
    await c1.shutdown();

    // Session 2 hits the persisted PCH pair: the preamble's links must be
    // served from the reloaded blob, not lost with the process.
    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait("main.cpp");
    const links2 = await c2.documentLinks(uri2);
    expect(
        (links2 ?? []).some((link) => (link.target ?? "").endsWith("foo.h")),
        "preamble document links lost across restart",
    ).toBe(true);
    expect(fs.statSync(workspace.pchFiles()[0]!).mtimeMs, "PCH was rebuilt instead of reused").toBe(
        pchMtime,
    );
    await c2.shutdown();
});

test("missing idx rebuilds pair", async ({ session }) => {
    const workspace = session.tmpdir();
    workspace.pinCacheDir();
    workspace.write("foo.h", "inline void foo() {}\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);

    const c1 = session.spawn(workspace);
    await c1.initialize(workspace);
    const [uri] = await c1.openAndWait("main.cpp");
    const locs = asLocations(await c1.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h"))).toBe(true);
    const pchMtime = fs.statSync(workspace.pchFiles()[0]!).mtimeMs;
    await c1.shutdown();

    // Half the pair vanishes (crash residue, external cleanup): the next
    // session must treat the PCH as a miss and rebuild both blobs.
    const idxFiles = workspace.pchIdxFiles();
    expect(idxFiles.length, "expected a committed .pch.idx next to the PCH").toBeGreaterThan(0);
    for (const idx of idxFiles) {
        fs.rmSync(idx);
    }
    await sleep(MTIME_GRANULARITY);

    const c2 = session.spawn(workspace);
    await c2.initialize(workspace);
    const [uri2] = await c2.openAndWait("main.cpp");
    const locs2 = asLocations(await c2.definitionAt(uri2, 1, 13));
    expect(
        locs2.some((loc) => loc.uri.endsWith("foo.h")),
        "overlay dead after losing the idx half of the pair",
    ).toBe(true);
    expect(
        fs.statSync(workspace.pchFiles()[0]!).mtimeMs,
        "PCH pair should have been rebuilt",
    ).not.toBe(pchMtime);
    expect(workspace.pchIdxFiles().length, "rebuilt pair is missing its idx blob").toBeGreaterThan(
        0,
    );
    await c2.shutdown();
});

test("header edit refreshes overlay", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("foo.h", "inline void foo() {}\n");
    workspace.write("main.cpp", '#include "foo.h"\nint main() { foo(); return 0; }\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, { initializationOptions: NO_INDEXING });

    const [uri] = await client.openAndWait("main.cpp");
    let locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.range.start.line === 0)).toBe(true);

    // Same preamble text, so the PCH key is unchanged; the header edit must
    // still refresh the pair (deps_changed) and the served overlay with it.
    await sleep(MTIME_GRANULARITY);
    workspace.write("foo.h", "// moved\ninline void foo() {}\n");
    await client.waitForRecompile(uri);

    locs = asLocations(await client.definitionAt(uri, 1, 13));
    expect(locs.some((loc) => loc.uri.endsWith("foo.h") && loc.range.start.line === 1)).toBe(true);
});
