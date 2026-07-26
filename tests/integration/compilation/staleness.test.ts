/// Integration tests for mtime-based staleness tracking.
///
/// Verifies that ensure_compiled() and ensure_pch() detect dependency file
/// changes via mtime snapshots, triggering recompilation without relying
/// on didSave to mark everything dirty.

import * as fs from "node:fs";
import * as path from "node:path";
import {
    assertCleanCompile,
    assertHasErrors,
    assertNoAnomaly,
    MTIME_GRANULARITY,
    SETTLE_TIME,
    sleep,
    waitForRecompile,
} from "../../tools/checks.ts";
import { withTimeout } from "../../tools/client.ts";
import { DATA_DIR, generateCdb, writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";
import { listPchFiles, makeTempWorkspace, pinCacheToWorkspace } from "../../tools/workspace.ts";

test("header change invalidates ast", async ({ session }) => {
    /// Modifying a header on disk should cause recompilation on next hover,
    /// even though didSave was never called (mtime-based detection).
    const { client, workspace } = session.tmp();
    // Setup: main.cpp includes header.h
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    // First compile — should succeed with no diagnostics.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Modify header on disk — introduce an error.
    // Ensure mtime advances past filesystem granularity (1s on some FSes).
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return }\n"); // syntax error

    // Send another hover — ensure_compiled should detect mtime change
    // in deps and trigger recompilation. The recompilation publishes
    // fresh diagnostics as a side effect.
    await waitForRecompile(client, uri);

    // Should now have diagnostics from the broken header.
    assertHasErrors(client, uri, "Expected diagnostics after header change");
});

test("header change invalidates pch", async ({ session }) => {
    /// Modifying a preamble header on disk should trigger PCH rebuild.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Foo { int x; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { Foo f; return f.x; }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    // First compile — success.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Modify header — rename struct field.
    // Ensure mtime advances past filesystem granularity (1s on some FSes).
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct Foo { int y; };\n"); // x -> y

    // Hover again — PCH should rebuild, AST should recompile.
    // main.cpp uses f.x which no longer exists → diagnostics expected.
    await waitForRecompile(client, uri, 30_000);

    assertHasErrors(client, uri, "Expected error after header field rename");
});

test("no change skips recompile", async ({ session }) => {
    /// When no dependency has changed, ensure_compiled should fast-path.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Second hover — should use cached AST (no recompilation).
    // Verify it returns quickly and doesn't crash.
    const hover = await client.hoverAt(uri, 0, 4);
    // "main" should be hoverable.
    expect(hover).not.toBeNull();
});

test("touch without content change skips recompile", async ({ session }) => {
    /// Layer 2: touching a header (mtime changes) without modifying content
    /// should NOT trigger recompilation — the hash check catches this.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Touch the header — mtime changes but content stays the same.
    await sleep(MTIME_GRANULARITY);
    const originalContent = fs.readFileSync(path.join(workspace, "header.h"), "utf8");
    fs.writeFileSync(path.join(workspace, "header.h"), originalContent);

    // Hover triggers ensure_compiled which runs deps_changed.
    // Layer 2 hash confirms nothing actually changed → cached AST reused.
    // The first hover may see ast_dirty=true (mtime changed, hash check in
    // progress), so retry to let the hash check complete.
    let hover = null;
    for (let i = 0; i < 3; i++) {
        hover = await client.hoverAt(uri, 1, 4);
        if (hover !== null) {
            break;
        }
        await sleep(SETTLE_TIME);
    }
    expect(hover).not.toBeNull();

    // No new diagnostics should appear — the file is still clean.
    assertCleanCompile(client, uri);
});

test("header replaced with different content", async ({ session }) => {
    /// Replacing a header file with different content should be detected
    /// and trigger recompilation reflecting the new content.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Replace header — delete and recreate with a breaking change.
    await sleep(MTIME_GRANULARITY);
    fs.rmSync(path.join(workspace, "header.h"));
    fs.writeFileSync(
        path.join(workspace, "header.h"),
        "inline int renamed_value() { return 1; }\n",
    );

    // main.cpp still calls value() which no longer exists → error.
    await waitForRecompile(client, uri);

    assertHasErrors(client, uri, "Expected diagnostics after header replacement");
});

test("fix error clears diagnostics", async ({ session }) => {
    /// After introducing and fixing an error in a header, diagnostics
    /// should clear on the next recompilation cycle.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return }\n"); // broken
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    // First compile — should produce diagnostics.
    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, uri, "Expected diagnostics from broken header");

    // Fix the header.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return 1; }\n");

    // Hover triggers recompilation — diagnostics should clear.
    await waitForRecompile(client, uri);

    assertCleanCompile(client, uri);
});

test("multiple files share header", async ({ session }) => {
    /// When a shared header changes, all open files that depend on it
    /// should detect the staleness independently.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "shared.h"), "inline int shared() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "a.cpp"),
        '#include "shared.h"\nint fa() { return shared(); }\n',
    );
    fs.writeFileSync(
        path.join(workspace, "b.cpp"),
        '#include "shared.h"\nint fb() { return shared(); }\n',
    );
    writeCdb(workspace, ["a.cpp", "b.cpp"]);
    await client.initialize(workspace);

    const [uriA] = await client.openAndWait(path.join(workspace, "a.cpp"));
    const [uriB] = await client.openAndWait(path.join(workspace, "b.cpp"));
    assertCleanCompile(client, uriA);
    assertCleanCompile(client, uriB);

    // Break the shared header.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "shared.h"), "inline int shared() { return }\n");

    // Both files should get diagnostics after hover.
    await waitForRecompile(client, uriA);
    assertHasErrors(client, uriA, "File A should have diagnostics");

    await waitForRecompile(client, uriB);
    assertHasErrors(client, uriB, "File B should have diagnostics");
});

test("transitive header change", async ({ session }) => {
    /// A change to a transitively included header should be detected.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "base.h"), "inline int base() { return 1; }\n");
    fs.writeFileSync(path.join(workspace, "mid.h"), '#include "base.h"\n');
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "mid.h"\nint main() { return base(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Modify the transitive dep (base.h).
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "base.h"), "inline int base() { return }\n"); // broken

    await waitForRecompile(client, uri);

    assertHasErrors(client, uri, "Expected diagnostics from transitive header change");
});

test("didchange body edit recompiles", async ({ session }) => {
    /// Editing the body (not preamble) via didChange should trigger
    /// recompilation and update diagnostics.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Introduce a body error via didChange.
    const arrived = client.armDiagnostics(uri);
    client.change(uri, 1, "int main() { return }\n"); // missing expression
    await client.hoverAt(uri, 0, 4);
    await withTimeout(arrived, 30_000, "diagnostics");

    assertHasErrors(client, uri, "Expected diagnostics after body error");
});

test("didchange preamble edit recompiles", async ({ session }) => {
    /// Changing a preamble #include via didChange should trigger PCH rebuild
    /// and recompilation reflecting the new header's declarations.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "a.h"),
        "#pragma once\ninline int from_a() { return 1; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "b.h"),
        "#pragma once\ninline int from_b() { return 2; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "a.h"\nint main() { return from_a(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Switch from a.h to b.h and call from_b() instead.
    const arrived = client.armDiagnostics(uri);
    client.change(uri, 1, '#include "b.h"\nint main() { return from_b(); }\n');
    await client.hoverAt(uri, 1, 4);
    await withTimeout(arrived, 30_000, "diagnostics");

    // Should compile cleanly — from_b() is available via b.h.
    assertCleanCompile(client, uri);
});

test("didclose then reopen", async ({ session }) => {
    /// Closing and reopening a file should work correctly — the server
    /// should not retain stale state from the previous session.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Close the file.
    client.close(uri);

    // Modify on disk while closed.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return }\n"); // broken

    // Reopen — should compile the new (broken) content from disk.
    const [uri2] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, uri2, "Expected diagnostics after reopen with broken content");
});

test("didclose clears hover", async ({ session }) => {
    /// After didClose, hover on the closed file should return an error.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));

    client.close(uri);

    await expect(withTimeout(client.hoverAt(uri, 0, 4), 10_000, "hover")).rejects.toThrow(
        "Document not open",
    );
});

test("didsave triggers recompile for dependents", async ({ session }) => {
    /// didSave on a header file should mark dependent documents dirty.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    // Modify header on disk and send didSave.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int value() { return }\n"); // broken
    client.save(client.pathToUri(path.join(workspace, "header.h")));

    // Hover should detect the change and recompile.
    await waitForRecompile(client, uri);

    assertHasErrors(client, uri, "Expected diagnostics after didSave on broken header");
});

test("didsave with module deps", async ({ session }) => {
    /// didSave on a module file should invalidate CompileGraph dependents.
    const { client, workspace } = session.tmp();
    const src = path.join(DATA_DIR, "modules", "save_recompile");
    for (const name of fs.readdirSync(src)) {
        const from = path.join(src, name);
        if (fs.statSync(from).isFile()) {
            fs.copyFileSync(from, path.join(workspace, name));
        }
    }

    generateCdb(workspace);
    await client.initialize(workspace);

    // Open and compile Mid (which imports Leaf).
    const [midUri] = await client.openAndWait(path.join(workspace, "mid.cppm"));
    assertCleanCompile(client, midUri);

    // Modify Leaf on disk and send didSave — should invalidate Mid's deps.
    fs.writeFileSync(
        path.join(workspace, "leaf.cppm"),
        "export module Leaf;\nexport int leaf() { return 999; }\n",
    );

    client.save(client.pathToUri(path.join(workspace, "leaf.cppm")));

    // Hover on Mid should trigger recompilation (Leaf PCM was invalidated).
    await waitForRecompile(client, midUri);

    assertCleanCompile(client, midUri);
});

test("flag change invalidates pch", async () => {
    /// Changing a -D flag in the CDB must produce a new PCH on the next
    /// session even though the preamble text is unchanged (flags are part of
    /// the cache key).
    const { workspace, track } = makeTempWorkspace();
    pinCacheToWorkspace(workspace);
    fs.writeFileSync(path.join(workspace, "header.h"), "#pragma once\nstruct F { int x; };\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { F f; return f.x; }\n',
    );

    // Session 1: build with -DFOO=1.
    writeCdb(workspace, ["main.cpp"], { extraArgs: ["-DFOO=1"] });
    const c1 = track(await makeClient(cliceExecutable(), workspace));
    const [uri] = await c1.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c1, uri);
    expect(listPchFiles(workspace).length).toBe(1);
    assertNoAnomaly(c1, workspace);
    await shutdownClient(c1);

    // Session 2: same preamble text, different flag — must not reuse.
    writeCdb(workspace, ["main.cpp"], { extraArgs: ["-DFOO=2"] });
    const c2 = track(await makeClient(cliceExecutable(), workspace));
    const [uri2] = await c2.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(c2, uri2);
    expect(
        listPchFiles(workspace).length,
        "A flag change must produce a second, separately keyed PCH",
    ).toBe(2);
    assertNoAnomaly(c2, workspace);
    await shutdownClient(c2);
});

test("host change resynthesizes preamble", async ({ session }) => {
    /// When the host source stops providing a dependency, the header's
    /// synthesized preamble must be rebuilt from the new disk state.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int get_x(Point p) { return p.x; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "types.h"\n#include "utils.h"\n' +
            "int main() { Point p{1, 2}; return get_x(p); }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));

    // utils.h has no CDB entry: compiled via automatic header context,
    // with types.h provided by the synthesized preamble from main.cpp.
    const [utilsUri] = await client.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(client, utilsUri);

    // Ensure mtime advances past filesystem granularity (1s on some FSes).
    await sleep(1_100);
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "utils.h"\nint main() { return 0; }\n',
    );

    // No didSave: mtime-based chain snapshot must detect the change.
    await waitForRecompile(client, utilsUri);
    assertHasErrors(client, utilsUri, "Expected errors after host stopped providing types.h");
});

test("intermediate change resynthesizes preamble", async ({ session }) => {
    /// Changing an intermediate file of the include chain (not the host)
    /// must also invalidate the synthesized preamble.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "wrapper.h"),
        '#pragma once\n#define VALUE 42\n#include "target.h"\n',
    );
    fs.writeFileSync(path.join(workspace, "target.h"), "inline int get() { return VALUE; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "wrapper.h"\nint main() { return get(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait(path.join(workspace, "main.cpp"));

    // target.h compiles via chain main.cpp -> wrapper.h, which provides VALUE.
    const [targetUri] = await client.openAndWait(path.join(workspace, "target.h"));
    assertCleanCompile(client, targetUri);

    // Rename the macro in the intermediate wrapper.h.
    await sleep(1_100);
    fs.writeFileSync(
        path.join(workspace, "wrapper.h"),
        '#pragma once\n#define OTHER 42\n#include "target.h"\n',
    );

    await waitForRecompile(client, targetUri);
    assertHasErrors(client, targetUri, "Expected errors after intermediate header changed");
});

test("saved host reinvalidates header", async ({ session }) => {
    /// didSave on a chain file must force preamble re-validation by content
    /// even when the file's mtime is unchanged (the pull path is blind then).
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "types.h"),
        "#pragma once\nstruct Point { int x; int y; };\n",
    );
    fs.writeFileSync(
        path.join(workspace, "utils.h"),
        "inline int get_x(Point p) { return p.x; }\n",
    );
    const mainCpp = path.join(workspace, "main.cpp");
    fs.writeFileSync(
        mainCpp,
        '#include "types.h"\n#include "utils.h"\n' +
            "int main() { Point p{1, 2}; return get_x(p); }\n",
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait(mainCpp);
    const [utilsUri] = await client.openAndWait(path.join(workspace, "utils.h"));
    assertCleanCompile(client, utilsUri);

    // Rewrite main.cpp but restore its original mtime: the mtime-based
    // Layer 1 check now cannot see the change, only the didSave push path
    // (which zeroes build_at, forcing a content re-hash) can catch it.
    const st = fs.statSync(mainCpp);
    fs.writeFileSync(mainCpp, '#include "utils.h"\nint main() { return 0; }\n');
    fs.utimesSync(mainCpp, st.atime, st.mtime);

    client.save(mainUri);

    await waitForRecompile(client, utilsUri);
    assertHasErrors(client, utilsUri, "Expected errors after didSave with restored mtime");
});

test("same second save detected", async ({ session }) => {
    /// A header saved immediately after the dependent's compile — within the
    /// same second — must still invalidate the PCH. Deliberately no mtime sleep:
    /// a watermark-based freshness check is blind exactly here.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(
        path.join(workspace, "header.h"),
        "#pragma once\ninline int value() { return 1; }\n",
    );
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    fs.writeFileSync(
        path.join(workspace, "header.h"),
        "#pragma once\ninline int renamed() { return 1; }\n",
    );
    client.save(client.pathToUri(path.join(workspace, "header.h")));

    // main.cpp still calls value(): a reused stale PCH would compile clean.
    await waitForRecompile(client, uri);
    assertHasErrors(client, uri, "Expected errors after same-second header save");
});

test("backdated header change detected", async ({ session }) => {
    /// A header whose content changes while its mtime moves backwards
    /// (rsync -t, git-restore-mtime) must be caught by the pull-side check
    /// alone — no didSave is sent.
    const { client, workspace } = session.tmp();
    const header = path.join(workspace, "header.h");
    fs.writeFileSync(header, "#pragma once\ninline int value() { return 1; }\n");
    fs.writeFileSync(
        path.join(workspace, "main.cpp"),
        '#include "header.h"\nint main() { return value(); }\n',
    );
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    assertCleanCompile(client, uri);

    const st = fs.statSync(header);
    fs.writeFileSync(header, "#pragma once\ninline int renamed() { return 1; }\n");
    fs.utimesSync(header, st.atime, new Date(st.mtimeMs - 100_000));

    await waitForRecompile(client, uri);
    assertHasErrors(client, uri, "Expected errors after backdated header change");
});

test("orphan header default command", async ({ session }) => {
    /// A header with no CDB entry and no including source falls back to the
    /// synthesized default command and still compiles.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    fs.writeFileSync(path.join(workspace, "orphan.h"), "inline int orphan_value() { return 7; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const [orphanUri] = await client.openAndWait(path.join(workspace, "orphan.h"));
    assertCleanCompile(client, orphanUri);
});

test("setup fail keeps dirty", async ({ session }) => {
    /// A compile that fails before parsing (bad target triple, no PCH to
    /// blame) must not settle: the gap is published as empty diagnostics and
    /// the next request recompiles instead of trusting the phantom product.
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    const entries = [
        {
            directory: workspace,
            file: path.join(workspace, "main.cpp"),
            arguments: [
                "clang++",
                "--target=bogus-unknown-none",
                "-fsyntax-only",
                path.join(workspace, "main.cpp"),
            ],
        },
    ];
    fs.writeFileSync(path.join(workspace, "compile_commands.json"), JSON.stringify(entries));
    await client.initialize(workspace);

    const [uri] = await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(client.diagnostics.get(uri) ?? [], "Honest gap must be empty").toEqual([]);

    // A settled phantom would serve the stale AST and never publish again;
    // a retained dirty flag recompiles and republishes on the next request.
    client.diagnostics.delete(uri);
    await waitForRecompile(client, uri, 30_000);
    expect(
        client.diagnostics.get(uri) ?? [],
        "The retried non-result must stay an honest empty gap",
    ).toEqual([]);
});
