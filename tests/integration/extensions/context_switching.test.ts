/// Integration tests for compilation context switching.
///
/// Covers source files with multiple CDB entries (selected by command hash),
/// include-occurrence contexts for guard-less headers, context deduplication
/// by canonical flags, host ranking, and switch validation.

import * as fs from "node:fs";
import * as path from "node:path";
import { MTIME_GRANULARITY, SETTLE_TIME, sleep } from "@clice/tools/client";
import { expect, test } from "../../fixtures.ts";

/// Snapshot the artifact directory as name -> mtime (nanoseconds), matching
/// the Python st_mtime_ns comparison.
function snapshotMtimes(dir: string): Record<string, bigint> {
    const out: Record<string, bigint> = {};
    for (const name of fs.readdirSync(dir)) {
        out[name] = fs.statSync(path.join(dir, name), { bigint: true }).mtimeNs;
    }
    return out;
}

/// Switching between two CDB entries of one source must recompile it
/// under the selected flags.
test("source command switch", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "#ifndef EXPECTED\n#error missing EXPECTED\n#endif\nint main() { return 0; }\n",
    );
    workspace.writeEntries([
        ["main.cpp", ["-DEXPECTED"]],
        ["main.cpp", []],
    ]);
    await client.initialize(workspace);

    // Default entry is the first one: EXPECTED defined, clean.
    const [mainUri] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(mainUri);

    const query = await client.queryContext(mainUri);
    expect(query.total).toBe(2);
    const contexts = query.contexts;
    expect(
        contexts.every((c) => c.commandHash!.length > 0),
        `Source contexts must carry commandHash, got: ${JSON.stringify(contexts)}`,
    ).toBe(true);
    const plainHash = contexts.find((c) => !c.label.includes("-DEXPECTED"))!.commandHash!;
    const definedHash = contexts.find((c) => c.label.includes("-DEXPECTED"))!.commandHash!;

    // Switch to the entry without the define: the #error must fire.
    let switched = await client.switchContext(mainUri, mainUri, { commandHash: plainHash });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(mainUri);
    client.assertHasErrors(mainUri, "Expected #error without -DEXPECTED");

    const current = await client.currentContext(mainUri);
    expect(current.context!.commandHash).toBe(plainHash);

    // And back.
    switched = await client.switchContext(mainUri, mainUri, { commandHash: definedHash });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(mainUri);
    client.assertCleanCompile(mainUri);
});

/// A guard-less header included twice by one host provides one context
/// per include occurrence.
test("occurrence switch", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\n");
    workspace.write(
        "main.cpp",
        "#define X(name) int name;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "#define X(name) void get_##name();\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    const [defUri] = await client.openAndWait("list.def");

    const query = await client.queryContext(defUri);
    expect(query.total, `Expected 2 occurrence contexts, got ${JSON.stringify(query)}`).toBe(2);
    const occurrences = query.contexts.map((c) => c.occurrence).sort((a, b) => (a ?? 0) - (b ?? 0));
    expect(
        occurrences,
        `Expected occurrences 0 and 1, got: ${JSON.stringify(query.contexts)}`,
    ).toEqual([0, 1]);

    // Pin each occurrence; both must compile cleanly and be reported back.
    for (const occ of [0, 1]) {
        const switched = await client.switchContext(defUri, mainUri, { occurrence: occ });
        expect(switched.success, `switch to occurrence ${occ}`).toBe(true);
        await client.waitForRecompile(defUri);
        client.assertCleanCompile(defUri);

        const current = await client.currentContext(defUri);
        expect(current.context!.occurrence).toBe(occ);
    }
});

/// Hosts with identical canonical flags collapse into one context, and
/// the representative is the best-ranked host (matching stem wins).
test("context dedup and ranking", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("widget.h", "inline int widget_size() { return 4; }\n");
    for (const name of ["zzz.cpp", "widget.cpp", "aaa.cpp"]) {
        workspace.write(name, `#include "widget.h"\nint ${name[0]!}() { return widget_size(); }\n`);
    }
    workspace.writeCDB(["zzz.cpp", "widget.cpp", "aaa.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("widget.cpp");
    // Dedup requires a confirmed self-contained verdict, earned by the
    // header's own trial compile — wait for it.
    const [widgetUri] = await client.openAndWait("widget.h");

    const query = await client.queryContext(widgetUri);
    expect(
        query.total,
        `Identical flags must dedupe to one context, got: ${JSON.stringify(query)}`,
    ).toBe(1);
    expect(
        query.contexts[0]!.uri.includes("widget.cpp"),
        `Representative should be the stem-matching host, got: ${JSON.stringify(query.contexts)}`,
    ).toBe(true);
});

/// Switching a header to a source that does not include it must fail.
test("switch rejects non includer", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("utils.h", "inline int util() { return 1; }\n");
    workspace.write("main.cpp", '#include "utils.h"\nint main() { return util(); }\n');
    workspace.write("other.cpp", "int other() { return 2; }\n");
    workspace.writeCDB(["main.cpp", "other.cpp"]);
    await client.initialize(workspace);

    await client.openAndWait("main.cpp");
    const [utilsUri] = client.open("utils.h");

    const otherUri = workspace.uri("other.cpp");
    const switched = await client.switchContext(utilsUri, otherUri);
    expect(switched.success, "Switching to a non-including host must be rejected").toBe(false);
});

/// Pinning an occurrence beyond the include count must be rejected.
test("occurrence out of range", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\n");
    workspace.write(
        "main.cpp",
        "#define X(name) int name;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    const [defUri] = client.open("list.def");

    const switched = await client.switchContext(defUri, mainUri, { occurrence: 5 });
    expect(switched.success, "Out-of-range occurrence must be rejected").toBe(false);
});

/// queryContext pages results: 12 distinct configs yield 10 + 2.
test("query context pagination", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("common.h", "inline int common() { return 1; }\n");
    const entries: [string, string[]][] = [];
    for (let n = 0; n < 12; n++) {
        const name = `s${String(n).padStart(2, "0")}.cpp`;
        workspace.write(name, `#include "common.h"\nint f${n}() { return common() + FLAVOR; }\n`);
        entries.push([name, [`-DFLAVOR=${n}`]]);
    }
    workspace.writeEntries(entries);
    await client.initialize(workspace);

    await client.openAndWait("s00.cpp");
    const [commonUri] = client.open("common.h");

    const first = await client.queryContext(commonUri);
    expect(first.total).toBe(12);
    expect(first.contexts.length).toBe(10);

    const second = await client.queryContext(commonUri, { offset: 10 });
    expect(second.total).toBe(12);
    expect(second.contexts.length).toBe(2);

    // The two pages must not overlap.
    const firstUris = new Set(first.contexts.map((c) => c.uri));
    const secondUris = second.contexts.map((c) => c.uri);
    expect(secondUris.some((u) => firstUris.has(u))).toBe(false);
});

/// A switch made against an outdated queryContext listing is rejected
/// with stale=true; re-querying yields a fresh epoch that works.
test("stale epoch rejected", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("shared.h", "VALUE_TYPE get_value();\n");
    workspace.write(
        "main.cpp",
        '#define VALUE_TYPE int\n#include "shared.h"\nint main() { return 0; }\n',
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    const [sharedUri] = client.open("shared.h");

    const query = await client.queryContext(sharedUri);
    const oldEpoch = query.epoch;
    expect(
        oldEpoch,
        `queryContext must stamp an epoch, got: ${JSON.stringify(query)}`,
    ).toBeTruthy();

    // Any save bumps the workspace epoch.
    client.save(mainUri);
    await sleep(500);

    let switched = await client.switchContext(sharedUri, mainUri, { epoch: oldEpoch });
    expect(switched.success).toBe(false);
    expect(switched.stale, `Expected stale rejection, got: ${JSON.stringify(switched)}`).toBe(true);

    const fresh = await client.queryContext(sharedUri);
    switched = await client.switchContext(sharedUri, mainUri, { epoch: fresh.epoch });
    expect(switched.success, `Fresh epoch must work, got: ${JSON.stringify(switched)}`).toBe(true);
});

/// A host built under several configurations provides one context per
/// CDB entry, switchable by command hash.
test("multi config host", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "render.h",
        "#pragma once\n" +
            "#if defined(USE_VULKAN)\n" +
            'inline const char* backend() { return "vk"; }\n' +
            "#elif defined(USE_METAL)\n" +
            'inline const char* backend() { return "mt"; }\n' +
            "#endif\n",
    );
    workspace.write("host.cpp", '#include "render.h"\nint main() { return backend()[0]; }\n');
    workspace.writeEntries([
        ["host.cpp", ["-DUSE_VULKAN"]],
        ["host.cpp", ["-DUSE_METAL"]],
    ]);
    await client.initialize(workspace);

    await client.openAndWait("host.cpp");
    const [renderUri] = await client.openAndWait("render.h");

    const query = await client.queryContext(renderUri);
    expect(query.total, JSON.stringify(query)).toBe(2);
    const contexts = query.contexts;
    const hashes = contexts.map((c) => c.commandHash);
    expect(
        hashes.every((h) => h !== undefined && h.length > 0) && new Set(hashes).size === 2,
        JSON.stringify(contexts),
    ).toBe(true);

    const metalHash = contexts.find((c) => c.label.includes("USE_METAL"))!.commandHash!;
    const hostUri = workspace.uri("host.cpp");
    const switched = await client.switchContext(renderUri, hostUri, { commandHash: metalHash });
    expect(switched.success, JSON.stringify(switched)).toBe(true);

    await client.waitForRecompile(renderUri);
    client.assertCleanCompile(renderUri);
    const current = await client.currentContext(renderUri);
    expect(current.context!.commandHash, JSON.stringify(current)).toBe(metalHash);
});

/// Adding an #include and saving must immediately expose the new host
/// in queryContext: the include graph is rescanned on didSave.
test("saved include updates hosts", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("lonely.h", "inline int lonely() { return 1; }\n");
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    const [lonelyUri] = client.open("lonely.h");

    let query = await client.queryContext(lonelyUri);
    expect(query.total, "No includers yet").toBe(0);

    // Include the header and save.
    const newText = '#include "lonely.h"\nint main() { return lonely(); }\n';
    workspace.write("main.cpp", newText);
    client.change(mainUri, 2, newText);
    client.save(mainUri);

    query = await client.queryContext(lonelyUri);
    expect(query.total, `New host must appear after save: ${JSON.stringify(query)}`).toBe(1);
    expect(query.contexts[0]!.uri).toContain("main.cpp");
});

/// Closing and reopening a header keeps its context choice and reuses
/// the synthesized preamble instead of re-synthesizing it.
test("reopen reuses preamble", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\nX(beta)\n");
    workspace.write(
        "main.cpp",
        "#define X(name) int name = 1;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "#define X(name) void get_##name();\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    let [defUri] = await client.openAndWait("list.def");

    const switched = await client.switchContext(defUri, mainUri, { occurrence: 1 });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(defUri);

    const artifactDir = workspace.path(path.join(".clice", "header_context"));
    const snapshot = snapshotMtimes(artifactDir);
    expect(Object.keys(snapshot).length, "expected synthesized preamble artifacts").toBeGreaterThan(
        0,
    );

    client.close(defUri);
    await sleep(MTIME_GRANULARITY);

    [defUri] = await client.openAndWait("list.def");
    const current = await client.currentContext(defUri);
    expect(current.context!.occurrence).toBe(1);

    const after = snapshotMtimes(artifactDir);
    expect(after, "reopen must reuse the preamble, not re-synthesize").toEqual(snapshot);
});

/// Reopening a header after its chain file changed on disk must NOT
/// reuse the stale preamble — the chain content is embedded in it.
test("chain change resynthesizes", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("list.def", "X(alpha)\nX(beta)\n");
    workspace.write(
        "main.cpp",
        "#define X(name) int name = 1;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "#define X(name) void get_##name();\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const [mainUri] = await client.openAndWait("main.cpp");
    let [defUri] = await client.openAndWait("list.def");

    const switched = await client.switchContext(defUri, mainUri, { occurrence: 1 });
    expect(switched.success).toBe(true);
    await client.waitForRecompile(defUri);

    const artifactDir = workspace.path(path.join(".clice", "header_context"));
    const snapshot = snapshotMtimes(artifactDir);
    expect(Object.keys(snapshot).length, "expected synthesized preamble artifacts").toBeGreaterThan(
        0,
    );

    client.close(defUri);
    await sleep(MTIME_GRANULARITY);
    // The chain file (the includer) changes on disk while the header is
    // closed: the embedded preamble content is now stale.
    workspace.write(
        "main.cpp",
        "#define X(name) int name = 2;\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "#define X(name) void get_##name();\n" +
            '#include "list.def"\n' +
            "#undef X\n" +
            "int main() { return alpha; }\n",
    );

    [defUri] = await client.openAndWait("list.def");
    const current = await client.currentContext(defUri);
    expect(current.context!.occurrence).toBe(1);

    const after = snapshotMtimes(artifactDir);
    expect(after, "stale preamble must be re-synthesized").not.toEqual(snapshot);
});

/// The client resync contract: after a successful switch the client
/// closes and reopens the document (the pull-based server only re-targets
/// the session); the reopened compile runs under the persisted choice.
test("switched context survives reopen", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write(
        "main.cpp",
        "#ifdef USE_B\nint broken() { return undefined_b_symbol; }\n#endif\n" +
            "int main() { return 0; }\n",
    );
    workspace.writeEntries([
        ["main.cpp", ["-DUSE_A"]],
        ["main.cpp", ["-DUSE_B"]],
    ]);
    await client.initialize(workspace);

    const [uri] = await client.openAndWait("main.cpp");
    client.assertCleanCompile(uri);

    const query = await client.queryContext(uri);
    const contexts = query.contexts;
    expect(query.total, `expected both entries: ${JSON.stringify(contexts)}`).toBe(2);
    const targetHash = contexts.find((c) => (c.label || "").includes("USE_B"))!.commandHash!;

    const switched = await client.switchContext(uri, uri, {
        commandHash: targetHash,
        epoch: query.epoch,
    });
    expect(switched.success, `switch failed: ${JSON.stringify(switched)}`).toBe(true);

    client.close(uri);
    await sleep(SETTLE_TIME);
    client.diagnostics.delete(uri);

    // The close publishes an empty retract that can race the reopen's
    // first publish; poll past it for the real compile's errors.
    const [uri2] = await client.openAndWait("main.cpp");
    for (let i = 0; i < 50; i++) {
        if (client.errors(uri2).length > 0) {
            break;
        }
        await sleep(200);
    }
    expect(
        (client.diagnostics.get(uri2) ?? []).some((d) =>
            (typeof d.message === "string" ? d.message : d.message.value).includes(
                "undefined_b_symbol",
            ),
        ),
        `expected the USE_B error after reopen: ${JSON.stringify(client.diagnostics.get(uri2) ?? [])}`,
    ).toBe(true);

    const current = await client.currentContext(uri2);
    expect(
        current.context!.commandHash,
        `persisted choice must survive the reopen: ${JSON.stringify(current)}`,
    ).toBe(targetHash);
});
