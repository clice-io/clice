/// File tracker: each test drives deterministic ticks through the
/// clice/internal/poll hook (loops disabled). The first workspace tick only
/// seeds the stat baseline, so tests poll once before mutating the disk.

import * as fs from "node:fs";
import * as path from "node:path";
import type { CliceClient } from "../../tools/client.ts";
import {
    getErrors,
    MTIME_GRANULARITY,
    sleep,
    waitForIndex,
    waitForRecompile,
    waitForReference,
    assertHasErrors,
    assertNoErrors,
} from "../../tools/checks.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { test, expect } from "../../tools/fixtures.ts";

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
    return ((await client.poll(loop)) as { events: number }).events;
}

test("cdb flag change recompiles", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), GATED_MAIN);
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, mainUri, "gate must fire without -DFEATURE");

    writeCdb(workspace, ["main.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    await waitForRecompile(client, mainUri);
    assertNoErrors(client, mainUri, "open file must pick up the new flags");
});

test("cdb new entry indexed", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));

    fs.writeFileSync(path.join(workspace, "lib.cpp"), "int lib_entry() { return 1; }\n");
    writeCdb(workspace, ["main.cpp", "lib.cpp"]);
    expect(await eventsOf(client, "cdb")).toBe(1);

    expect(
        await waitForIndex(client, mainUri, "lib_entry"),
        "file added to the CDB was never indexed",
    ).toBe(true);
});

test("cdb removed entry recheck", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), "inline int shared() { return 0; }\n");
    fs.writeFileSync(path.join(workspace, "gone.cpp"), '#include "header.h"\n');
    writeCdb(workspace, ["gone.cpp"]);
    await client.initialize(workspace);

    const headerUri = client.pathToUri(path.join(workspace, "header.h"));
    let result = (await client.queryContext(headerUri)) as { total: number };
    expect(result.total, "gone.cpp must host the header initially").toBeGreaterThanOrEqual(1);

    writeCdb(workspace, []);
    expect(await eventsOf(client, "cdb")).toBe(1);

    result = (await client.queryContext(headerUri)) as { total: number };
    expect(result.total, "removed entry must stop hosting the header").toBe(0);
});

test("cdb appears after startup", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), GATED_MAIN);
    fs.writeFileSync(path.join(workspace, "lib.cpp"), "int lib_entry() { return 1; }\n");
    await client.initialize(workspace);

    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, mainUri, "guessed command cannot define FEATURE");

    // The editor was opened first; cmake runs later.
    writeCdb(workspace, ["main.cpp", "lib.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    await waitForRecompile(client, mainUri);
    assertNoErrors(client, mainUri, "open file must switch to the discovered CDB");
    expect(
        await waitForIndex(client, mainUri, "lib_entry"),
        "closed file from the discovered CDB was never indexed",
    ).toBe(true);
});

test("checkout updates workspace", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), HEADER_V1);
    const mainV1 =
        '#include "header.h"\nstatic_assert(VALUE == 2, "");\nint main() { return 0; }\n';
    fs.writeFileSync(path.join(workspace, "main.cpp"), mainV1);
    const closedV1 = '#include "header.h"\nint use_target() { return TARGET(); }\n';
    fs.writeFileSync(path.join(workspace, "closed.cpp"), closedV1);
    writeCdb(workspace, ["main.cpp", "closed.cpp"]);
    await client.initialize(workspace);

    const headerUri = client.pathToUri(path.join(workspace, "header.h"));
    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    const closedUri = client.pathToUri(path.join(workspace, "closed.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, mainUri, "static_assert must fire against header V1");
    expect(
        await waitForReference(client, headerUri, 2, 11, closedUri),
        "initial index never resolved the closed TU's alpha call",
    ).toBe(true);

    expect(await eventsOf(client, "workspace")).toBe(0); // seeding sweep

    // Simulate git checkout: rewrite files on disk, no didSave.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), HEADER_V2);
    fs.writeFileSync(
        path.join(workspace, "closed.cpp"),
        closedV1 + "int checkout_added() { return 3; }\n",
    );
    expect(await eventsOf(client, "workspace")).toBe(2);

    await waitForRecompile(client, mainUri);
    assertNoErrors(client, mainUri, "open file must compile against the new header");
    expect(
        await waitForReference(client, headerUri, 3, 11, closedUri),
        "closed TU was not reindexed against the new header",
    ).toBe(true);
    expect(
        await waitForIndex(client, mainUri, "checkout_added"),
        "closed TU's own disk change was not indexed",
    ).toBe(true);
});

test("touch emits no events", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "header.h"), HEADER_V1);
    fs.writeFileSync(path.join(workspace, "main.cpp"), '#include "header.h"\n');
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace);

    expect(await eventsOf(client, "workspace")).toBe(0); // seeding sweep

    // mtime bump, identical bytes: the content-hash check must stay silent.
    await sleep(MTIME_GRANULARITY);
    fs.writeFileSync(path.join(workspace, "header.h"), HEADER_V1);
    expect(await eventsOf(client, "workspace")).toBe(0);
});

test("cdb polling loop live", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), GATED_MAIN);
    writeCdb(workspace, ["main.cpp"]);
    await client.initialize(workspace, {
        initializationOptions: { tracker: { cdb_poll_seconds: 1 } },
    });

    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));
    assertHasErrors(client, mainUri);

    writeCdb(workspace, ["main.cpp"], { extraArgs: ["-DFEATURE"] });
    // No hook: the 1s poll loop needs two stable ticks (settle debounce),
    // so poll for the errors to clear instead of trusting one fixed sleep.
    // Until the reload lands the hover fast-paths on a clean AST and no
    // diagnostics arrive — that round just times out and retries.
    for (let i = 0; i < 30; i++) {
        await sleep(1_000);
        try {
            await waitForRecompile(client, mainUri, 3_000);
        } catch {
            continue;
        }
        if (getErrors(client.diagnostics.get(mainUri) ?? []).length === 0) {
            break;
        }
    }
    assertNoErrors(client, mainUri, "the polling loop must reload the CDB on its own");
});

test("cdb flag change reindexes closed", async ({ session }) => {
    const { client, workspace } = session.tmp();
    fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
    fs.writeFileSync(path.join(workspace, "lib.cpp"), GATED_LIB);
    writeCdb(workspace, ["main.cpp", "lib.cpp"]);
    await client.initialize(workspace);

    const mainUri = client.pathToUri(path.join(workspace, "main.cpp"));
    await client.openAndWait(path.join(workspace, "main.cpp"));
    expect(
        await waitForIndex(client, mainUri, "feature_off"),
        "closed file was never indexed initially",
    ).toBe(true);

    // Only lib.cpp's flags change; its bytes do not. Content-based staleness
    // cannot see this — the CDB delta must force the reindex.
    writeCdb(workspace, ["main.cpp", "lib.cpp"], { extraArgs: ["-DFEATURE"] });
    expect(await eventsOf(client, "cdb")).toBe(1);

    expect(
        await waitForIndex(client, mainUri, "feature_on"),
        "closed file was not reindexed after its flags changed",
    ).toBe(true);
});
