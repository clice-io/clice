/// File tracker: each test drives deterministic ticks through the
/// clice/internal/poll hook (loops disabled). The first workspace tick only
/// seeds the stat baseline, so tests poll once before mutating the disk.

import { MTIME_GRANULARITY, sleep, type CliceClient } from "@clice/tools/client";
import { test, expect } from "../fixtures.ts";

const GATED_MAIN = `#ifndef FEATURE
#error missing FEATURE
#endif
int main() { return 0; }
`;

const HEADER_V1 = `#define VALUE 1
#define TARGET alpha
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const HEADER_V2 = `#define VALUE 2
#define TARGET beta
inline int alpha() { return 1; }
inline int beta() { return 2; }
`;

const GATED_LIB = `#ifdef FEATURE
int feature_on() { return 1; }
#else
int feature_off() { return 0; }
#endif
`;

async function eventsOf(client: CliceClient, loop: "cdb" | "workspace"): Promise<number> {
    return (await client.poll(loop)).events;
}

test("cdb flag change recompiles", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", GATED_MAIN);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const mainUri = workspace.uri("main.cpp");
    await client.openAndWait("main.cpp");
    client.assertHasErrors(mainUri, "gate must fire without -DFEATURE");

    workspace.writeCDB(["main.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    await client.waitForRecompile(mainUri);
    client.assertNoErrors(mainUri, "open file must pick up the new flags");
});

test("cdb new entry indexed", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    const mainUri = workspace.uri("main.cpp");
    await client.openAndWait("main.cpp");

    workspace.write("lib.cpp", "int lib_entry() { return 1; }\n");
    workspace.writeCDB(["main.cpp", "lib.cpp"]);
    expect(await eventsOf(client, "cdb")).toBe(1);

    expect(
        await client.waitForIndex(mainUri, "lib_entry"),
        "file added to the CDB was never indexed",
    ).toBe(true);
});

test("cdb removed entry recheck", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("header.h", "inline int shared() { return 0; }\n");
    workspace.write("gone.cpp", '#include "header.h"\n');
    workspace.writeCDB(["gone.cpp"]);
    await client.initialize(workspace);

    const headerUri = workspace.uri("header.h");
    let result = await client.queryContext(headerUri);
    expect(result.total, "gone.cpp must host the header initially").toBeGreaterThanOrEqual(1);

    workspace.writeCDB([]);
    expect(await eventsOf(client, "cdb")).toBe(1);

    result = await client.queryContext(headerUri);
    expect(result.total, "removed entry must stop hosting the header").toBe(0);
});

test("cdb appears after startup", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", GATED_MAIN);
    workspace.write("lib.cpp", "int lib_entry() { return 1; }\n");
    await client.initialize(workspace);

    const mainUri = workspace.uri("main.cpp");
    await client.openAndWait("main.cpp");
    client.assertHasErrors(mainUri, "guessed command cannot define FEATURE");

    // The editor was opened first; cmake runs later.
    workspace.writeCDB(["main.cpp", "lib.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    await client.waitForRecompile(mainUri);
    client.assertNoErrors(mainUri, "open file must switch to the discovered CDB");
    expect(
        await client.waitForIndex(mainUri, "lib_entry"),
        "closed file from the discovered CDB was never indexed",
    ).toBe(true);
});

test("checkout updates workspace", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("header.h", HEADER_V1);
    const mainV1 =
        '#include "header.h"\nstatic_assert(VALUE == 2, "");\nint main() { return 0; }\n';
    workspace.write("main.cpp", mainV1);
    const closedV1 = '#include "header.h"\nint use_target() { return TARGET(); }\n';
    workspace.write("closed.cpp", closedV1);
    workspace.writeCDB(["main.cpp", "closed.cpp"]);
    await client.initialize(workspace);

    const headerUri = workspace.uri("header.h");
    const mainUri = workspace.uri("main.cpp");
    const closedUri = workspace.uri("closed.cpp");
    await client.openAndWait("main.cpp");
    client.assertHasErrors(mainUri, "static_assert must fire against header V1");
    expect(
        await client.waitForReference(headerUri, 2, 11, closedUri),
        "initial index never resolved the closed TU's alpha call",
    ).toBe(true);

    expect(await eventsOf(client, "workspace")).toBe(0); // seeding sweep

    // Simulate git checkout: rewrite files on disk, no didSave.
    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", HEADER_V2);
    workspace.write("closed.cpp", closedV1 + "int checkout_added() { return 3; }\n");
    expect(await eventsOf(client, "workspace")).toBe(2);

    await client.waitForRecompile(mainUri);
    client.assertNoErrors(mainUri, "open file must compile against the new header");
    expect(
        await client.waitForReference(headerUri, 3, 11, closedUri),
        "closed TU was not reindexed against the new header",
    ).toBe(true);
    expect(
        await client.waitForIndex(mainUri, "checkout_added"),
        "closed TU's own disk change was not indexed",
    ).toBe(true);
});

test("touch emits no events", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("header.h", HEADER_V1);
    workspace.write("main.cpp", '#include "header.h"\n');
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace);

    expect(await eventsOf(client, "workspace")).toBe(0); // seeding sweep

    // mtime bump, identical bytes: the content-hash check must stay silent.
    await sleep(MTIME_GRANULARITY);
    workspace.write("header.h", HEADER_V1);
    expect(await eventsOf(client, "workspace")).toBe(0);
});

test("cdb polling loop live", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", GATED_MAIN);
    workspace.writeCDB(["main.cpp"]);
    await client.initialize(workspace, {
        initializationOptions: { tracker: { cdb_poll_seconds: 1 } },
    });

    const mainUri = workspace.uri("main.cpp");
    await client.openAndWait("main.cpp");
    client.assertHasErrors(mainUri);

    workspace.writeCDB(["main.cpp"], { extraArgs: ["-DFEATURE"] });
    // No hook: the 1s poll loop needs two stable ticks (settle debounce),
    // so poll for the errors to clear instead of trusting one fixed sleep.
    // Until the reload lands the hover fast-paths on a clean AST and no
    // diagnostics arrive — that round just times out and retries.
    for (let i = 0; i < 30; i++) {
        await sleep(1_000);
        try {
            await client.waitForRecompile(mainUri, 3_000);
        } catch {
            continue;
        }
        if (client.errors(mainUri).length === 0) {
            break;
        }
    }
    client.assertNoErrors(mainUri, "the polling loop must reload the CDB on its own");
});

test("cdb flag change reindexes closed", async ({ session }) => {
    const { client, workspace } = session.tmp();
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.write("lib.cpp", GATED_LIB);
    workspace.writeCDB(["main.cpp", "lib.cpp"]);
    await client.initialize(workspace);

    const mainUri = workspace.uri("main.cpp");
    await client.openAndWait("main.cpp");
    expect(
        await client.waitForIndex(mainUri, "feature_off"),
        "closed file was never indexed initially",
    ).toBe(true);

    // Only lib.cpp's flags change; its bytes do not. Content-based staleness
    // cannot see this — the CDB delta must force the reindex.
    workspace.writeCDB(["main.cpp", "lib.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    expect(
        await client.waitForIndex(mainUri, "feature_on"),
        "closed file was not reindexed after its flags changed",
    ).toBe(true);
});
