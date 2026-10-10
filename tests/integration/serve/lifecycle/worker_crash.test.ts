/// What a document shows and does when a worker crashes on it: the crash
/// note on the file, no retry while the file sits still, a retry after an
/// edit (spaced, and bounded) or a save, and no blame for the documents a
/// crash merely takes along.

import type * as proto from "vscode-languageserver-protocol";
import type { Launch, Serve } from "@clice/tools/actions";
import { sleep } from "@clice/tools/client";
import type { Manifest } from "@clice/tools/project";
import { at, expect, serve } from "../../fixtures.ts";

const NOTE = "clice's worker crashed";
const HEALTHY = "int add(int a, int b) { return a + b; }\n";
const FIXED = "int add(int a, int b) { return a + b; }\n// fixed\n";
/// Past the server's retry spacing, a clock rule: a change earns a retry
/// only this long after the crash.
const RETRY_SPACING = 2_100;
const HANG =
    "constexpr long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n" +
    "constexpr long x = fib(90);\n";

/// Past the preamble: the stateful compile itself crashes.
function poison(n: number): string {
    return `${HEALTHY}// edit ${n}\n#pragma clang __debug crash\n`;
}

/// The worker crashes are the point here, and the compiler keeps
/// `#pragma clang __debug crash` live. clang-tidy, whose crash a compile
/// first retries without (clang_tidy.test.ts), stays off; so does indexing,
/// whose rows of the disk text would answer the requests a crashed compile
/// leaves unanswered. A crash request names its file by a part of its tag:
/// the workspace's path is unknown before the case starts.
function crashing(
    files: Record<string, string>,
    options: {
        env?: Record<string, string>;
        project?: Record<string, unknown>;
        manifest?: Manifest;
        launch?: Launch;
    } = {},
) {
    return serve.files(files, {
        config: {
            diagnostics: { clang_tidy: false },
            project: { enable_indexing: false, ...options.project },
        },
        env: { CLICE_TEST_PRAGMA_CRASH: "1", ...options.env },
        anomalies: true,
        ...(options.manifest === undefined ? {} : { manifest: options.manifest }),
        ...(options.launch === undefined ? {} : { launch: options.launch }),
    });
}

function message(diagnostic: proto.Diagnostic): string {
    return typeof diagnostic.message === "string" ? diagnostic.message : diagnostic.message.value;
}

function notes(diagnostics: proto.Diagnostic[]): string[] {
    return diagnostics.map(message).filter((text) => text.includes(NOTE));
}

/// The crash note of `file` that names `fragment`, as the file stands.
async function note(s: Serve, file: string, fragment: string): Promise<string> {
    const shown = notes(await s.diagnostics(file));
    const found = shown.find((text) => text.includes(fragment));
    expect(found, `a crash note with "${fragment}" among ${JSON.stringify(shown)}`).toBeDefined();
    return found ?? "";
}

/// Whether a push to `file` ever carried a crash note.
function everNoted(s: Serve, file: string): boolean {
    const uri = s.uri(file);
    return s.client.publishedDiagnostics.some(
        (params) =>
            s.client.normalizeUri(params.uri) === uri &&
            params.diagnostics.some((d) => message(d).includes(NOTE)),
    );
}

/// The workers that crashed on `kind` requests (of `file`), once the
/// server settled.
async function crashes(s: Serve, kind: string, file?: string): Promise<number> {
    await s.sync();
    return s.workspace.workerCrashes(
        file === undefined ? kind : `${kind} ${s.workspace.displayPath(file)}`,
    );
}

const ADD = (file: string) => at(file, "int ad|d(");

crashing({ "poison.cpp": poison(0) })("compile crash waits for save", async ({ s }) => {
    s.open("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    const shown = await note(s, "poison.cpp", "while compiling this file");
    expect(shown).toContain("save it");
    expect(shown).toMatch(
        /killed by signal \d+ \(SIG[A-Z]+\)|terminated by exception 0x[0-9A-F]{8} \(/,
    );
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");

    // A file that sits still is never retried, whatever is asked of it.
    for (let i = 0; i < 3; i++) {
        expect(await s.hover(ADD("poison.cpp"))).toBeNull();
        await s.request("textDocument/documentSymbol", "poison.cpp");
    }
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);

    // A save is the user's retry: exactly one more attempt.
    s.save("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(2);
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(2);
});

crashing(
    { "poison.cpp": poison(0) },
    // A client that pulls gets no pushes.
    { launch: { capabilities: { textDocument: { diagnostic: {} } } } },
)("pull shows the crash note", async ({ s }) => {
    s.open("poison.cpp", { pull: false });
    const pulled = (await s.diagnostics("poison.cpp")).map(message);
    expect(pulled).toEqual([expect.stringContaining("while compiling this file")]);
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
    expect(await s.pushed("poison.cpp")).toBeUndefined();
});

crashing({ "poison.cpp": poison(0) })("edit retries after a pause", async ({ s }) => {
    s.open("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    await note(s, "poison.cpp", "while compiling this file");

    // The fix needs no save: past the retry spacing, the next request
    // compiles it, and the note goes with the crash.
    s.edit("poison.cpp", { text: FIXED });
    await sleep(RETRY_SPACING);
    expect(await s.hover(ADD("poison.cpp"))).not.toBeNull();
    expect(notes(await s.diagnostics("poison.cpp"))).toEqual([]);
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);
});

crashing({ "poison.cpp": HEALTHY })("editing crash is bounded", async ({ s }) => {
    await s.compiled("poison.cpp");

    // Half-typed code crashing is the common case while editing: the first
    // crash stays silent.
    s.edit("poison.cpp", { text: poison(1) });
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);
    expect(everNoted(s, "poison.cpp")).toBe(false);

    // Each later edit earns one spaced retry; a repeat shows.
    await sleep(RETRY_SPACING);
    s.edit("poison.cpp", { text: poison(2) });
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(2);
    expect(await note(s, "poison.cpp", "2 times in a row")).toContain("changes");

    await sleep(RETRY_SPACING);
    s.edit("poison.cpp", { text: poison(3) });
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(3);
    expect(await note(s, "poison.cpp", "3 times in a row")).toContain("until you save this file");

    // Out of strikes: edits no longer retry, a save does.
    await sleep(RETRY_SPACING);
    s.edit("poison.cpp", { text: poison(4) });
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(3);
    s.save("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(4);
});

crashing({ "main.cpp": HEALTHY }, { env: { CLICE_TEST_CRASH_REQUEST: "query:Hover " } })(
    "query crash pauses that feature",
    async ({ s }) => {
        s.open("main.cpp");
        expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
        expect(await s.hover(ADD("main.cpp"))).toBeNull();
        await note(s, "main.cpp", "while computing hover for this file");
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(1);

        // The compile and every other feature carry on; hover alone no
        // longer reaches a worker.
        expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
        expect(notes(await s.diagnostics("main.cpp")).length).toBe(1);
        expect(await s.hover(ADD("main.cpp"))).toBeNull();
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(1);

        s.save("main.cpp");
        expect(await s.hover(ADD("main.cpp"))).toBeNull();
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(2);
    },
);

crashing(
    { "main.cpp": `${HEALTHY}int x = ad;\n` },
    { env: { CLICE_TEST_CRASH_REQUEST: "completion " } },
)("completion crash pauses completion", async ({ s }) => {
    const complete = () => s.completion(at("main.cpp", "int x = ad|;"));
    s.open("main.cpp");
    expect(await s.hover(ADD("main.cpp"))).not.toBeNull();
    await complete();
    await note(s, "main.cpp", "while completing code in this file");
    expect(await crashes(s, "completion", "main.cpp")).toBe(1);

    expect(await complete()).toBeNull();
    expect(await s.hover(ADD("main.cpp"))).not.toBeNull();
    expect(await crashes(s, "completion", "main.cpp")).toBe(1);
});

const PREAMBLE_POISON = `#pragma clang __debug crash\n${HEALTHY}`;

crashing({ "poison.cpp": PREAMBLE_POISON, "twin.cpp": PREAMBLE_POISON, "healthy.cpp": HEALTHY })(
    "preamble crash is shared",
    async ({ s }) => {
        await s.compiled("healthy.cpp");
        s.open("poison.cpp");
        expect(await s.hover(ADD("poison.cpp"))).toBeNull();
        await note(s, "poison.cpp", "while building the precompiled preamble of this file");
        expect(await crashes(s, "buildPch", "poison.cpp")).toBe(1);

        // A document with the same preamble learns the crash without one of
        // its own.
        s.open("twin.cpp");
        expect(await s.hover(ADD("twin.cpp"))).toBeNull();
        await note(s, "twin.cpp", "precompiled preamble");
        expect(await crashes(s, "buildPch")).toBe(1);

        expect(await s.hover(ADD("healthy.cpp"))).not.toBeNull();

        // The fixed preamble, saved, comes back.
        s.edit("poison.cpp", { text: FIXED });
        s.save("poison.cpp");
        expect(await s.hover(ADD("poison.cpp"))).not.toBeNull();
    },
);

crashing(
    {
        "math.cppm":
            "export module Math;\n\nexport int add(int a, int b) {\n    return a + b;\n}\n",
        "main.cpp": "import Math;\n\nint main() {\n    return add(1, 2);\n}\n",
        "other.cpp": "import Math;\nint other() { return add(3, 4); }\n",
    },
    { env: { CLICE_TEST_CRASH_REQUEST: "buildPcm " } },
)("module crash notes importers", async ({ s }) => {
    const call = at("main.cpp", "return a|dd(1, 2)");
    s.open("main.cpp");
    await s.hover(call);
    await note(s, "main.cpp", "while building a module imported by this file");
    expect(await crashes(s, "buildPcm", "math.cppm")).toBe(1);

    // The importer still compiles — its parse reports the missing module —
    // but the module is not rebuilt until the importer changes or saves.
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);
    await s.hover(call);
    expect(await crashes(s, "buildPcm", "math.cppm")).toBe(1);
    s.save("main.cpp");
    await s.hover(call);
    expect(await crashes(s, "buildPcm", "math.cppm")).toBe(2);

    // Another importer learns the crash without one of its own.
    const otherCall = at("other.cpp", "add(3|, 4)");
    s.open("other.cpp");
    await s.hover(otherCall);
    await note(s, "other.cpp", "while building a module imported by this file");
    expect(await crashes(s, "buildPcm", "math.cppm")).toBe(2);

    // Edited, the module is built again for it without a save.
    s.disk.write("math.cppm", `${s.disk.read("math.cppm")}// edited\n`);
    await s.sync({ poll: true });
    await s.hover(otherCall);
    expect(await crashes(s, "buildPcm", "math.cppm")).toBe(3);
});

crashing(
    { "poison.h": "#pragma once\nint known();\n", "main.cpp": `#include "poison.h"\n${HEALTHY}` },
    { env: { CLICE_TEST_CRASH_REQUEST: "buildPch " } },
)("preamble crash heals with a header", async ({ s }) => {
    s.open("main.cpp");
    expect(await s.hover(ADD("main.cpp"))).toBeNull();
    await note(s, "main.cpp", "precompiled preamble");
    expect(await crashes(s, "buildPch", "main.cpp")).toBe(1);
    expect(await s.hover(ADD("main.cpp"))).toBeNull();
    expect(await crashes(s, "buildPch", "main.cpp")).toBe(1);

    // A change to a header the preamble includes is a retry, with no save
    // of the file itself.
    await sleep(RETRY_SPACING);
    s.disk.write("poison.h", "#pragma once\nint known();\nint more();\n");
    await s.sync({ poll: true });
    await s.hover(ADD("main.cpp"));
    expect(await crashes(s, "buildPch", "main.cpp")).toBe(2);
});

crashing(
    { "header.h": "#pragma once\nint known();\n", "main.cpp": `#include "header.h"\n${HEALTHY}` },
    { env: { CLICE_TEST_CRASH_REQUEST: "compile " } },
)("crash reading a preamble rebuilds it", async ({ s }) => {
    // The first crash may be a corrupt preamble's: the pair is rebuilt and
    // the compile rerun once, and only that crash is the file's.
    s.open("main.cpp");
    expect(await s.hover(ADD("main.cpp"))).toBeNull();
    expect(await note(s, "main.cpp", "while compiling this file")).not.toContain("times in a row");
    expect(await crashes(s, "compile", "main.cpp")).toBe(2);
    expect(
        s.workspace.log("master.log").split("Compile crashed consuming PCH pair").length - 1,
    ).toBe(1);
});

crashing(
    { "healthy.cpp": HEALTHY, "poison.cpp": poison(0) },
    // One thread to compile on: the healthy compile queues behind the
    // poison's and is still in flight when the worker dies. One stateful
    // worker hosts both documents.
    { env: { UV_THREADPOOL_SIZE: "1" }, project: { stateful_worker_count: 1 } },
)("victims are not blamed", async ({ s }) => {
    const compiles = (name: string) =>
        s.workspace.log("SF-0.log").split(`Compile request: path=${s.workspace.displayPath(name)}`)
            .length - 1;

    await s.compiled("healthy.cpp");
    s.open("poison.cpp");
    for (let round = 1; round <= 3; round++) {
        // The healthy document's compile is taken along by the poison's
        // crash: it is resent, not blamed.
        const started = compiles("healthy.cpp");
        const poisoned = s.hover(ADD("poison.cpp"));
        // Any later reply: the server took up the poison's compile before
        // the healthy edit arrives.
        await s.counts();
        s.edit("healthy.cpp", { text: `${HEALTHY}// round ${round}\n` });
        expect(await s.hover(ADD("healthy.cpp")), `round ${round}`).not.toBeNull();
        expect(await poisoned).toBeNull();
        expect(await crashes(s, "compile", "poison.cpp")).toBe(round);
        expect(compiles("healthy.cpp")).toBeGreaterThanOrEqual(started + 2);
        s.save("poison.cpp");
    }
    expect(everNoted(s, "healthy.cpp")).toBe(false);
    expect(notes(await s.diagnostics("poison.cpp")).length).toBe(1);
});

/// Hung compiles: they end only when their worker dies.
const HUNG: Manifest = {
    cxx: ["-std=c++23"],
    units: Object.fromEntries(
        ["a.cpp", "b.cpp", "c.cpp"].map((name) => [name, ["-fconstexpr-steps=2147483647"]]),
    ),
};

crashing(
    { "a.cpp": HANG, "b.cpp": HANG, "c.cpp": HANG },
    // One stateful worker compiles all three side by side.
    { manifest: HUNG, project: { stateful_worker_count: 1 } },
).skipIf(process.platform !== "linux")("shared deaths blame nobody", async ({ s }) => {
    const names = Object.keys(HUNG.units);
    for (const name of names) {
        s.open(name);
    }
    const answers = names.map((name) => s.hover(at(name, "long f|ib(")));
    // Killed twice, the second time while all three resends compile: the
    // death names none of them and they shared the worker, so none is
    // blamed.
    for (const round of [1, 2]) {
        // What the server still runs at the deadline of its own sync is
        // in flight: the compiles, which never end by themselves.
        const { pending } = await s.client.sync({ deadlineMs: 1_000 });
        expect(pending, `round ${round}`).toEqual(
            expect.arrayContaining(names.map((name) => `compile ${s.workspace.displayPath(name)}`)),
        );
        for (const pid of s.client.workerPids("SF-")) {
            process.kill(pid, "SIGKILL");
        }
    }
    await Promise.all(answers);
    for (const name of names) {
        expect(everNoted(s, name), name).toBe(false);
    }
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
});

crashing({ "poison.cpp": poison(0) })("reopen keeps the bar", async ({ s }) => {
    s.open("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    await note(s, "poison.cpp", "while compiling this file");
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);

    // Closing and reopening the same bytes is no retry; the note is back
    // at once.
    s.close("poison.cpp");
    expect(notes((await s.pushed("poison.cpp")) ?? [])).toEqual([]);
    s.open("poison.cpp");
    await note(s, "poison.cpp", "while compiling this file");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);

    s.save("poison.cpp");
    expect(await s.hover(ADD("poison.cpp"))).toBeNull();
    expect(await crashes(s, "compile", "poison.cpp")).toBe(2);
});

crashing(
    { "hang.cpp": HANG },
    {
        env: { CLICE_TEST_REQUEST_DEADLINE_MS: "2000" },
        manifest: {
            cxx: ["-std=c++23"],
            units: { "hang.cpp": ["-fconstexpr-steps=2147483647"] },
        },
    },
)("hung compile is killed", async ({ s }) => {
    const call = at("hang.cpp", "long f|ib(");
    s.open("hang.cpp");
    expect(await s.hover(call)).toBeNull();
    expect(await note(s, "hang.cpp", "while compiling this file")).toContain(
        "killed after running for over 2 seconds",
    );

    // Barred like any crash: no second hang.
    expect(await s.hover(call)).toBeNull();
    await s.sync();
    expect(s.workspace.log("master.log").split("for over 2s; killing it").length - 1).toBe(1);
});
