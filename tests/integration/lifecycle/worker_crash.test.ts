/// What a document shows and does when a worker crashes on it: the crash
/// note on the file, no retry while the file sits still, a retry after an
/// edit (spaced, and bounded) or a save, and no blame for the documents a
/// crash merely takes along.

import * as path from "node:path";
import type * as proto from "vscode-languageserver-protocol";
import {
    MTIME_GRANULARITY,
    SLOW_SOURCE,
    sleep,
    waitUntil,
    type CliceClient,
} from "@clice/tools/client";
import { DATA_DIR } from "@clice/tools/compile-commands";
import type { Workspace } from "@clice/tools/workspace";
import { expect, test, type SessionFactory } from "../fixtures.ts";

const NOTE = "clice's worker crashed";
const HEALTHY = "int add(int a, int b) { return a + b; }\n";
const FIXED = "int add(int a, int b) { return a + b; }\n// fixed\n";
const RETRY_SPACING = 2_100;

/// Past the preamble: the stateful compile itself crashes.
function poison(n: number): string {
    return `${HEALTHY}// edit ${n}\n#pragma clang __debug crash\n`;
}

/// The worker crashes are the point here: the session opts out of the
/// anomaly gate, Debug builds must not trap on them, and the compiler keeps
/// `#pragma clang __debug crash` live.
function crashing(env: Record<string, string> = {}) {
    return {
        allowAnomaly: true,
        env: { CLICE_ANOMALY_NO_TRAP: "1", CLICE_TEST_PRAGMA_CRASH: "1", ...env },
    };
}

/// A workspace whose crashes are the compile's own: clang-tidy, whose
/// crash a compile first retries without (clang_tidy.test.ts), stays off.
function crashWorkspace(session: SessionFactory): Workspace {
    const workspace = session.tmpdir();
    workspace.write("clice.toml", "[diagnostics]\nclang_tidy = false\n");
    return workspace;
}

function text(diagnostic: proto.Diagnostic): string {
    return typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value;
}

function notes(client: CliceClient, uri: string): string[] {
    return (client.diagnostics.get(uri) ?? [])
        .map(text)
        .filter((message) => message.includes(NOTE));
}

function everNoted(client: CliceClient, uri: string): boolean {
    return client.publishedDiagnostics.some(
        (params) =>
            client.normalizeUri(params.uri) === uri &&
            params.diagnostics.some((d) => text(d).includes(NOTE)),
    );
}

async function waitNote(client: CliceClient, uri: string, fragment: string): Promise<string> {
    let found = "";
    await waitUntil(
        () => {
            found = notes(client, uri).find((note) => note.includes(fragment)) ?? "";
            return found !== "";
        },
        { timeout: 20_000, interval: 200, description: `a crash note with "${fragment}"` },
    );
    return found;
}

async function settleCrashes(workspace: Workspace, tag: string, count: number): Promise<void> {
    await waitUntil(() => workspace.workerCrashes(tag) >= count, {
        timeout: 20_000,
        interval: 100,
        description: `${count} crashes of ${tag}`,
    });
    expect(workspace.workerCrashes(tag)).toBe(count);
}

test("compile crash waits for save", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["poison.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace);
    const compile = `compile ${workspace.displayPath("poison.cpp")}`;

    const [uri] = client.open("poison.cpp");
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    const note = await waitNote(client, uri, "while compiling this file");
    expect(note).toContain("save it");
    expect(note).toMatch(
        /killed by signal \d+ \(SIG[A-Z]+\)|terminated by exception 0x[0-9A-F]{8} \(/,
    );
    await settleCrashes(workspace, compile, 1);
    expect(workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");

    // A file that sits still is never retried, whatever is asked of it.
    for (let i = 0; i < 3; i++) {
        expect(await client.hoverAt(uri, 0, 5)).toBeNull();
        await client.documentSymbols(uri);
    }
    expect(workspace.workerCrashes(compile)).toBe(1);

    // A save is the user's retry: exactly one more attempt.
    client.save(uri);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 2);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    expect(workspace.workerCrashes(compile)).toBe(2);
});

test("pull shows the crash note", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["poison.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace, { capabilities: { textDocument: { diagnostic: {} } } });

    const [uri] = client.open("poison.cpp");
    const pulled = (await client.pullDiagnostics(uri)).map(text);
    expect(pulled).toEqual([expect.stringContaining("while compiling this file")]);
    await settleCrashes(workspace, `compile ${workspace.displayPath("poison.cpp")}`, 1);
    expect(workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
    expect(client.publishCount(uri)).toBe(0);
});

test("edit retries after a pause", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["poison.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace);

    const [uri] = client.open("poison.cpp");
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await waitNote(client, uri, "while compiling this file");

    // The fix needs no save: past the retry spacing, the next request
    // compiles it, and the note goes with the crash.
    client.change(uri, 1, FIXED);
    const recovered = await waitUntil(() => client.hoverAt(uri, 0, 5), {
        timeout: 20_000,
        interval: 500,
        description: "the fixed document to recover",
    });
    expect(recovered).not.toBeNull();
    await waitUntil(() => notes(client, uri).length === 0, {
        timeout: 10_000,
        interval: 200,
        description: "the crash note to go",
    });
    expect(workspace.workerCrashes(`compile ${workspace.displayPath("poison.cpp")}`)).toBe(1);
});

test("editing crash is bounded", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.cpp", HEALTHY);
    workspace.writeCDB(["poison.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace);
    const compile = `compile ${workspace.displayPath("poison.cpp")}`;
    const [uri] = await client.openAndWait("poison.cpp");

    // Half-typed code crashing is the common case while editing: the first
    // crash stays silent.
    client.change(uri, 1, poison(1));
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 1);
    expect(everNoted(client, uri)).toBe(false);

    // Each later edit earns one spaced retry; a repeat shows.
    await sleep(RETRY_SPACING);
    client.change(uri, 2, poison(2));
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 2);
    expect(await waitNote(client, uri, "2 times in a row")).toContain("changes");

    await sleep(RETRY_SPACING);
    client.change(uri, 3, poison(3));
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 3);
    expect(await waitNote(client, uri, "3 times in a row")).toContain("until you save this file");

    // Out of strikes: edits no longer retry, a save does.
    await sleep(RETRY_SPACING);
    client.change(uri, 4, poison(4));
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    expect(workspace.workerCrashes(compile)).toBe(3);
    client.save(uri);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 4);
});

test("query crash pauses that feature", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("main.cpp", HEALTHY);
    workspace.writeCDB(["main.cpp"]);
    const hover = `query:Hover ${workspace.displayPath("main.cpp")}`;
    const client = session.spawn(workspace, crashing({ CLICE_TEST_CRASH_REQUEST: hover }));
    await client.initialize(workspace);

    const [uri] = client.open("main.cpp");
    expect(await client.semanticTokensFull(uri)).not.toBeNull();
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await waitNote(client, uri, "while computing hover for this file");
    await settleCrashes(workspace, hover, 1);

    // The compile and every other feature carry on; hover alone no longer
    // reaches a worker.
    const compiled = client.armDiagnostics(uri);
    expect(await client.semanticTokensFull(uri)).not.toBeNull();
    await compiled;
    expect(notes(client, uri).length).toBe(1);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    expect(workspace.workerCrashes(hover)).toBe(1);

    client.save(uri);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, hover, 2);
});

test("completion crash pauses completion", async ({ session }) => {
    const workspace = crashWorkspace(session);
    const text = `${HEALTHY}int x = ad;\n`;
    workspace.write("main.cpp", text);
    workspace.writeCDB(["main.cpp"]);
    const completion = `completion ${workspace.displayPath("main.cpp")}`;
    const client = session.spawn(workspace, crashing({ CLICE_TEST_CRASH_REQUEST: completion }));
    await client.initialize(workspace);

    const [uri] = client.open("main.cpp");
    expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
    await client.completionAt(uri, 1, 10);
    await waitNote(client, uri, "while completing code in this file");
    await settleCrashes(workspace, completion, 1);

    expect(await client.completionAt(uri, 1, 10)).toBeNull();
    expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
    expect(workspace.workerCrashes(completion)).toBe(1);
});

test("preamble crash is shared", async ({ session }) => {
    const workspace = crashWorkspace(session);
    const preamble = `#pragma clang __debug crash\n${HEALTHY}`;
    workspace.write("poison.cpp", preamble);
    workspace.write("twin.cpp", preamble);
    workspace.write("healthy.cpp", HEALTHY);
    workspace.writeCDB(["poison.cpp", "twin.cpp", "healthy.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace);
    const build = `buildPch ${workspace.displayPath("poison.cpp")}`;

    const [healthyUri] = await client.openAndWait("healthy.cpp");
    const [uri] = client.open("poison.cpp");
    expect(await client.hoverAt(uri, 1, 5)).toBeNull();
    await waitNote(client, uri, "while building the precompiled preamble of this file");
    await settleCrashes(workspace, build, 1);

    // A document with the same preamble learns the crash without one of
    // its own.
    const [twinUri] = client.open("twin.cpp");
    expect(await client.hoverAt(twinUri, 1, 5)).toBeNull();
    await waitNote(client, twinUri, "precompiled preamble");
    expect(workspace.workerCrashes("buildPch")).toBe(1);

    expect(await client.hoverAt(healthyUri, 0, 5)).not.toBeNull();

    // The fixed preamble, saved, comes back.
    client.change(uri, 1, FIXED);
    client.save(uri);
    const recovered = await waitUntil(() => client.hoverAt(uri, 0, 5), {
        timeout: 20_000,
        interval: 500,
        description: "the fixed preamble to recover",
    });
    expect(recovered).not.toBeNull();
});

test("module crash notes importers", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.copyFiles(path.join(DATA_DIR, "modules", "consumer_imports_module"));
    workspace.write("other.cpp", "import Math;\nint other() { return add(3, 4); }\n");
    workspace.write(
        "CMakeLists.txt",
        workspace.read("CMakeLists.txt").replace("PRIVATE main.cpp", "PRIVATE main.cpp other.cpp"),
    );
    workspace.generateCDB();
    const build = `buildPcm ${workspace.displayPath("math.cppm")}`;
    const client = session.spawn(workspace, crashing({ CLICE_TEST_CRASH_REQUEST: build }));
    await client.initialize(workspace);

    const [uri] = client.open("main.cpp");
    await client.hoverAt(uri, 3, 12);
    await waitNote(client, uri, "while building a module imported by this file");
    await settleCrashes(workspace, build, 1);

    // The importer still compiles — its parse reports the missing module —
    // but the module is not rebuilt until the importer changes or saves.
    await waitUntil(() => client.errors(uri).length > 0, {
        timeout: 20_000,
        interval: 200,
        description: "the importer's parse to report the missing module",
    });
    await client.hoverAt(uri, 3, 12);
    expect(workspace.workerCrashes(build)).toBe(1);
    client.save(uri);
    await client.hoverAt(uri, 3, 12);
    await settleCrashes(workspace, build, 2);

    // Another importer learns the crash without one of its own.
    const [otherUri] = client.open("other.cpp");
    await client.hoverAt(otherUri, 1, 26);
    await waitNote(client, otherUri, "while building a module imported by this file");
    expect(workspace.workerCrashes(build)).toBe(2);

    // Edited, the module is built again for it without a save.
    await sleep(MTIME_GRANULARITY);
    workspace.write("math.cppm", `${workspace.read("math.cppm")}// edited\n`);
    await client.poll("workspace");
    await client.hoverAt(otherUri, 1, 26);
    await settleCrashes(workspace, build, 3);
});

test("preamble crash heals with a header", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.h", "#pragma once\nint known();\n");
    workspace.write("main.cpp", `#include "poison.h"\n${HEALTHY}`);
    workspace.writeCDB(["main.cpp"]);
    const build = `buildPch ${workspace.displayPath("main.cpp")}`;
    const client = session.spawn(workspace, crashing({ CLICE_TEST_CRASH_REQUEST: build }));
    await client.initialize(workspace);

    const [uri] = client.open("main.cpp");
    expect(await client.hoverAt(uri, 1, 5)).toBeNull();
    await waitNote(client, uri, "precompiled preamble");
    await settleCrashes(workspace, build, 1);
    expect(await client.hoverAt(uri, 1, 5)).toBeNull();
    expect(workspace.workerCrashes(build)).toBe(1);

    // A change to a header the preamble includes is a retry, with no save
    // of the file itself.
    await sleep(RETRY_SPACING);
    workspace.write("poison.h", "#pragma once\nint known();\nint more();\n");
    await client.poll("workspace");
    await client.hoverAt(uri, 1, 5);
    await settleCrashes(workspace, build, 2);
});

test("crash reading a preamble rebuilds it", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("header.h", "#pragma once\nint known();\n");
    workspace.write("main.cpp", `#include "header.h"\n${HEALTHY}`);
    workspace.writeCDB(["main.cpp"]);
    const compile = `compile ${workspace.displayPath("main.cpp")}`;
    const client = session.spawn(workspace, crashing({ CLICE_TEST_CRASH_REQUEST: compile }));
    await client.initialize(workspace);

    // The first crash may be a corrupt preamble's: the pair is rebuilt and
    // the compile rerun once, and only that crash is the file's.
    const [uri] = client.open("main.cpp");
    expect(await client.hoverAt(uri, 1, 5)).toBeNull();
    const note = await waitNote(client, uri, "while compiling this file");
    expect(note).not.toContain("times in a row");
    await settleCrashes(workspace, compile, 2);
    expect(workspace.log("master.log").split("Compile crashed consuming PCH pair").length - 1).toBe(
        1,
    );
});

test("victims are not blamed", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("healthy.cpp", HEALTHY);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["healthy.cpp", "poison.cpp"]);
    // One thread to compile on: the healthy compile queues behind the
    // poison's and is still in flight when the worker dies.
    const client = session.spawn(workspace, crashing({ UV_THREADPOOL_SIZE: "1" }));
    // One stateful worker hosts both documents.
    await client.initialize(workspace, {
        initializationOptions: { project: { stateful_worker_count: 1 } },
    });
    const compile = `compile ${workspace.displayPath("poison.cpp")}`;
    const compiles = (name: string) =>
        workspace.log("SF-0.log").split(`Compile request: path=${workspace.displayPath(name)}`)
            .length - 1;

    const [healthyUri] = await client.openAndWait("healthy.cpp");
    const [uri] = client.open("poison.cpp");
    for (let round = 1; round <= 3; round++) {
        // The healthy document's compile is taken along by the poison's
        // crash: it is resent, not blamed.
        const started = compiles("healthy.cpp");
        const poisoned = client.hoverAt(uri, 0, 5);
        await waitUntil(() => compiles("poison.cpp") >= round, {
            timeout: 20_000,
            interval: 10,
            description: `the poison compile of round ${round} to start`,
        });
        client.change(healthyUri, round, `${HEALTHY}// round ${round}\n`);
        expect(await client.hoverAt(healthyUri, 0, 5), `round ${round}`).not.toBeNull();
        expect(await poisoned).toBeNull();
        await settleCrashes(workspace, compile, round);
        expect(compiles("healthy.cpp")).toBeGreaterThanOrEqual(started + 2);
        client.save(uri);
    }
    expect(everNoted(client, healthyUri)).toBe(false);
    expect(notes(client, uri).length).toBe(1);
});

test.skipIf(process.platform !== "linux")("shared deaths blame nobody", async ({ session }) => {
    const workspace = crashWorkspace(session);
    const names = ["a.cpp", "b.cpp", "c.cpp"];
    for (const name of names) {
        workspace.write(name, SLOW_SOURCE);
    }
    workspace.writeCDB(names);
    const client = session.spawn(workspace, crashing());
    // One stateful worker compiles all three side by side.
    await client.initialize(workspace, {
        initializationOptions: { project: { stateful_worker_count: 1 } },
    });
    const compiles = () => workspace.log("SF-0.log").split("Compile request:").length - 1;

    const uris = names.map((name) => client.open(name)[0]);
    const answers = uris.map((uri) => client.hoverAt(uri, 0, 5));
    // Killed twice, the second time while all three resends compile: the
    // death names none of them and they shared the worker, so none is blamed.
    for (const round of [1, 2]) {
        await waitUntil(() => compiles() >= names.length * round, {
            timeout: 20_000,
            interval: 10,
            description: `round ${round} of the compiles to start`,
        });
        for (const pid of client.workerPids("SF-")) {
            process.kill(pid, "SIGKILL");
        }
    }
    await Promise.all(answers);
    for (const uri of uris) {
        expect(everNoted(client, uri), uri).toBe(false);
    }
    expect(workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
});

test("reopen keeps the bar", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write("poison.cpp", poison(0));
    workspace.writeCDB(["poison.cpp"]);
    const client = session.spawn(workspace, crashing());
    await client.initialize(workspace);
    const compile = `compile ${workspace.displayPath("poison.cpp")}`;

    let [uri] = client.open("poison.cpp");
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await waitNote(client, uri, "while compiling this file");
    await settleCrashes(workspace, compile, 1);

    // Closing and reopening the same bytes is no retry; the note is back
    // at once.
    client.close(uri);
    await waitUntil(() => notes(client, uri).length === 0, {
        timeout: 10_000,
        interval: 200,
        description: "the close to retract the note",
    });
    [uri] = client.open("poison.cpp");
    await waitNote(client, uri, "while compiling this file");
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    expect(workspace.workerCrashes(compile)).toBe(1);

    client.save(uri);
    expect(await client.hoverAt(uri, 0, 5)).toBeNull();
    await settleCrashes(workspace, compile, 2);
});

test("hung compile is killed", async ({ session }) => {
    const workspace = crashWorkspace(session);
    workspace.write(
        "hang.cpp",
        "constexpr long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n" +
            "constexpr long x = fib(90);\n",
    );
    workspace.writeCDB(["hang.cpp"], { extraArgs: ["-fconstexpr-steps=2147483647"] });
    const client = session.spawn(workspace, crashing({ CLICE_TEST_REQUEST_DEADLINE_MS: "2000" }));
    await client.initialize(workspace, {
        initializationOptions: { project: { enable_indexing: false } },
    });

    const [uri] = client.open("hang.cpp");
    expect(await client.hoverAt(uri, 0, 16)).toBeNull();
    const note = await waitNote(client, uri, "while compiling this file");
    expect(note).toContain("killed after running for over 2 seconds");

    // Barred like any crash: no second hang.
    expect(await client.hoverAt(uri, 0, 16)).toBeNull();
    expect(workspace.log("master.log").split("for over 2s; killing it").length - 1).toBe(1);
});
