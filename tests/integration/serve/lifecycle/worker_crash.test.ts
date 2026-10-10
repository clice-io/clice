/// What a document shows and does when a worker crashes on it: the crash
/// note on the file, no retry while the file sits still, a retry after an
/// edit (spaced, and bounded) or a save, and no blame for the documents a
/// crash merely takes along.

import type * as proto from "vscode-languageserver-protocol";
import type { Serve, ServeOptions } from "@clice/tools/actions";
import { sleep } from "@clice/tools/client";
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
    options: Pick<ServeOptions, "manifest" | "launch" | "crashOn"> & {
        env?: Record<string, string>;
        project?: Record<string, unknown>;
    } = {},
): ServeOptions {
    const { env, project, ...rest } = options;
    return {
        config: {
            diagnostics: { clang_tidy: false },
            project: { enable_indexing: false, ...project },
        },
        env: { CLICE_TEST_PRAGMA_CRASH: "1", ...env },
        anomalies: true,
        ...rest,
    };
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

serve.files({ "poison.cpp": poison(0) }, crashing())(
    "compile crash waits for save",
    async ({ s }) => {
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
    },
);

serve.files(
    { "poison.cpp": poison(0) },
    // A client that pulls gets no pushes.
    crashing({ launch: { capabilities: { textDocument: { diagnostic: {} } } } }),
)("pull shows the crash note", async ({ s }) => {
    s.open("poison.cpp", { pull: false });
    const pulled = (await s.diagnostics("poison.cpp")).map(message);
    expect(pulled).toEqual([expect.stringContaining("while compiling this file")]);
    expect(await crashes(s, "compile", "poison.cpp")).toBe(1);
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
    expect(await s.pushed("poison.cpp")).toBeUndefined();
});

serve.files({ "poison.cpp": poison(0) }, crashing())(
    "edit retries after a pause",
    async ({ s }) => {
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
    },
);

serve("tiny", crashing())("editing crash is bounded", async ({ s }) => {
    await s.compiled("main.cpp");
    const add = at("main.cpp", "int ad|d(");
    // Past the preamble: the stateful compile itself crashes.
    const strike = (n: number) => {
        s.edit("main.cpp", {
            after: "return value - 3;\n}\n",
            insert: `// edit ${n}\n#pragma clang __debug crash\n`,
        });
    };

    // Half-typed code crashing is the common case while editing: the first
    // crash stays silent.
    strike(1);
    expect(await s.hover(add)).toBeNull();
    expect(await crashes(s, "compile", "main.cpp")).toBe(1);
    expect(everNoted(s, "main.cpp")).toBe(false);

    // Each later edit earns one spaced retry; a repeat shows.
    await sleep(RETRY_SPACING);
    strike(2);
    expect(await s.hover(add)).toBeNull();
    expect(await crashes(s, "compile", "main.cpp")).toBe(2);
    expect(await note(s, "main.cpp", "2 times in a row")).toContain("changes");

    await sleep(RETRY_SPACING);
    strike(3);
    expect(await s.hover(add)).toBeNull();
    expect(await crashes(s, "compile", "main.cpp")).toBe(3);
    expect(await note(s, "main.cpp", "3 times in a row")).toContain("until you save this file");

    // Out of strikes: edits no longer retry, a save does.
    await sleep(RETRY_SPACING);
    strike(4);
    expect(await s.hover(add)).toBeNull();
    expect(await crashes(s, "compile", "main.cpp")).toBe(3);
    s.save("main.cpp");
    expect(await s.hover(add)).toBeNull();
    expect(await crashes(s, "compile", "main.cpp")).toBe(4);
});

serve("tiny", crashing({ env: { CLICE_TEST_CRASH_REQUEST: "query:Hover " } }))(
    "query crash pauses that feature",
    async ({ s }) => {
        const add = at("main.cpp", "int ad|d(");
        s.open("main.cpp");
        expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
        expect(await s.hover(add)).toBeNull();
        await note(s, "main.cpp", "while computing hover for this file");
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(1);

        // The compile and every other feature carry on; hover alone no
        // longer reaches a worker.
        expect(await s.request("textDocument/semanticTokens/full", "main.cpp")).not.toBeNull();
        expect(notes(await s.diagnostics("main.cpp")).length).toBe(1);
        expect(await s.hover(add)).toBeNull();
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(1);

        s.save("main.cpp");
        expect(await s.hover(add)).toBeNull();
        expect(await crashes(s, "query:Hover", "main.cpp")).toBe(2);
    },
);

serve("tiny", crashing({ env: { CLICE_TEST_CRASH_REQUEST: "completion " } }))(
    "completion crash pauses completion",
    async ({ s }) => {
        const add = at("main.cpp", "int ad|d(");
        const complete = () => s.completion(at("main.cpp", "value = ad|d(1, 2)"));
        s.open("main.cpp");
        expect(await s.hover(add)).not.toBeNull();
        await complete();
        await note(s, "main.cpp", "while completing code in this file");
        expect(await crashes(s, "completion", "main.cpp")).toBe(1);

        expect(await complete()).toBeNull();
        expect(await s.hover(add)).not.toBeNull();
        expect(await crashes(s, "completion", "main.cpp")).toBe(1);
    },
);

const PREAMBLE_POISON = `#pragma clang __debug crash\n${HEALTHY}`;

serve.files(
    { "poison.cpp": PREAMBLE_POISON, "twin.cpp": PREAMBLE_POISON, "healthy.cpp": HEALTHY },
    crashing(),
)("preamble crash is shared", async ({ s }) => {
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
});

serve("shapes/modules", crashing({ crashOn: { request: "buildPcm", file: "src/shapes.cppm" } }))(
    "module crash notes importers",
    async ({ s }) => {
        const main = s.file("main");
        const call = at(s.file("main"), "shapes::ar|ea(c)");
        s.open(main);
        await s.hover(call);
        await note(s, main, "while building a module imported by this file");
        expect(await crashes(s, "buildPcm", "src/shapes.cppm")).toBe(1);

        // The importer still compiles — its parse reports the missing
        // module — but the module is not rebuilt until the importer changes
        // or saves.
        expect((await s.errors(main)).length).toBeGreaterThan(0);
        await s.hover(call);
        expect(await crashes(s, "buildPcm", "src/shapes.cppm")).toBe(1);
        s.save(main);
        await s.hover(call);
        expect(await crashes(s, "buildPcm", "src/shapes.cppm")).toBe(2);

        // Another importer learns the crash without one of its own.
        const demo = s.file("demo");
        const otherCall = at(s.file("demo"), "unit.meas|ure()");
        s.open(demo);
        await s.hover(otherCall);
        await note(s, demo, "while building a module imported by this file");
        expect(await crashes(s, "buildPcm", "src/shapes.cppm")).toBe(2);

        // Edited, the module is built again for it without a save.
        s.disk.edit("src/shapes.cppm", { after: "export module shapes;\n", insert: "// edited\n" });
        await s.sync({ poll: true });
        await s.hover(otherCall);
        expect(await crashes(s, "buildPcm", "src/shapes.cppm")).toBe(3);
    },
);

serve("shapes/headers", crashing({ env: { CLICE_TEST_CRASH_REQUEST: "buildPch " } }))(
    "preamble crash heals with a header",
    async ({ s }) => {
        const main = s.file("main");
        const circle = at(s.file("main"), "shapes::Cir|cle c(");
        s.open(main);
        expect(await s.hover(circle)).toBeNull();
        await note(s, main, "precompiled preamble");
        expect(await crashes(s, "buildPch", main)).toBe(1);
        expect(await s.hover(circle)).toBeNull();
        expect(await crashes(s, "buildPch", main)).toBe(1);

        // A change to a header the preamble includes is a retry, with no
        // save of the file itself.
        await sleep(RETRY_SPACING);
        s.disk.edit(s.file("circle"), {
            after: "double area(const Circle& circle);\n",
            insert: "\ndouble more();\n",
        });
        await s.sync({ poll: true });
        await s.hover(circle);
        expect(await crashes(s, "buildPch", main)).toBe(2);
    },
);

serve("shapes/headers", crashing({ env: { CLICE_TEST_CRASH_REQUEST: "compile " } }))(
    "crash reading a preamble rebuilds it",
    async ({ s }) => {
        // The first crash may be a corrupt preamble's: the pair is rebuilt
        // and the compile rerun once, and only that crash is the file's.
        const main = s.file("main");
        s.open(main);
        expect(await s.hover(at(s.file("main"), "shapes::Cir|cle c("))).toBeNull();
        expect(await note(s, main, "while compiling this file")).not.toContain("times in a row");
        expect(await crashes(s, "compile", main)).toBe(2);
        expect(
            s.workspace.log("master.log").split("Compile crashed consuming PCH pair").length - 1,
        ).toBe(1);
    },
);

serve.files(
    { "healthy.cpp": HEALTHY, "poison.cpp": poison(0) },
    // One stateful worker hosts both documents.
    crashing({ project: { stateful_worker_count: 1 } }),
)("victims are not blamed", async ({ s }) => {
    await s.compiled("healthy.cpp");
    s.open("poison.cpp", { pull: false });
    for (let round = 1; round <= 3; round++) {
        // The healthy document's compile runs beside the poison's when the
        // poison crashes the worker: it is resent, not blamed.
        const started = (await s.counts()).files["healthy.cpp"]?.compile ?? 0;
        const poison = await s.gate("compile", "poison.cpp");
        const healthy = await s.gate("compile", "healthy.cpp");
        const poisoned = s.hover(ADD("poison.cpp"));
        await poison.reached();
        s.edit("healthy.cpp", { text: `${HEALTHY}// round ${round}\n` });
        const answered = s.hover(ADD("healthy.cpp"));
        await healthy.reached();
        await poison.release();
        expect(await answered, `round ${round}`).not.toBeNull();
        expect(await poisoned).toBeNull();
        await healthy.release();
        expect(await crashes(s, "compile", "poison.cpp")).toBe(round);
        // Run in the worker that died, and again.
        expect((await s.counts()).files["healthy.cpp"]?.compile).toBeGreaterThanOrEqual(
            started + 2,
        );
        s.save("poison.cpp");
    }
    expect(everNoted(s, "healthy.cpp")).toBe(false);
    expect(notes(await s.diagnostics("poison.cpp")).length).toBe(1);
});

serve(
    "shapes/headers",
    // One stateful worker compiles all three side by side.
    crashing({ project: { stateful_worker_count: 1 } }),
).skipIf(process.platform !== "linux")("shared deaths blame nobody", async ({ s }) => {
    const units = [
        at(s.file("registry"), "int registry_co|unt() {"),
        at(s.file("circle_impl"), "const char* Circle::na|me() const {"),
        at(s.file("polygon_impl"), "double ed|ge(double length) {"),
    ];
    const gated = () => Promise.all(units.map(({ file }) => s.gate("compile", file)));
    let running = await gated();
    for (const { file } of units) {
        s.open(file, { pull: false });
    }
    const answers = units.map((loc) => s.hover(loc));
    // Killed twice, the second time while all three resends compile: the
    // death names none of them and they shared the worker, so none is
    // blamed.
    for (const round of [1, 2]) {
        await Promise.all(running.map((gate) => gate.reached()));
        const resends = round === 1 ? await gated() : [];
        for (const pid of s.client.workerPids("SF-")) {
            process.kill(pid, "SIGKILL");
        }
        await Promise.all(running.map((gate) => gate.release()));
        running = resends;
    }
    await Promise.all(answers);
    for (const { file } of units) {
        expect(everNoted(s, file), file).toBe(false);
    }
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
});

serve.files({ "poison.cpp": poison(0) }, crashing())("reopen keeps the bar", async ({ s }) => {
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

serve.files(
    { "hang.cpp": HANG },
    crashing({
        env: { CLICE_TEST_REQUEST_DEADLINE_MS: "2000" },
        manifest: {
            cxx: ["-std=c++23"],
            units: { "hang.cpp": ["-fconstexpr-steps=2147483647"] },
        },
    }),
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
