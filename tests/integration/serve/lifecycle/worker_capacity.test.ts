/// Requests ride out windows without a worker — a restart backoff, an LRU
/// eviction — instead of answering empty or clearing what the file shows.

import type * as proto from "vscode-languageserver-protocol";
import { at, expect, serve } from "../../fixtures.ts";

serve
    .files(
        { "main.cpp": 'int add(int a, int b) { return a + b; }\nint x = "s";\n' },
        // Without an index, a hover waits for the worker: rows of the
        // file would answer it during the outage.
        {
            config: { project: { stateful_worker_count: 1, enable_indexing: false } },
            anomalies: true,
        },
    )
    .skipIf(process.platform !== "linux")("outage keeps diagnostics", async ({ s }) => {
    expect((await s.errors("main.cpp")).length).toBeGreaterThan(0);
    const published = (await s.pushes("main.cpp")).length;

    // Fast deaths push the lone stateful worker into its respawn
    // backoff. SIGUSR2 names no request and, unlike a kill from outside
    // (SIGKILL), counts as the worker failing by itself. A compile after
    // each waits for the worker to come back instead of answering empty;
    // the last one waits out the longest backoff.
    let previous = 0;
    for (let kill = 1; kill <= 3; kill++) {
        const [pid] = s.client.workerPids("SF-");
        expect(pid, `a stateful worker before kill ${kill}`).toBeDefined();
        expect(pid).not.toBe(previous);
        previous = pid ?? 0;
        // The signal lands when it lands: a request sent before the
        // master saw the death could still be answered by the worker.
        const died = s.client.nextLogMessage((message) =>
            message.includes("[anomaly:WorkerCrash]"),
        );
        process.kill(previous, "SIGUSR2");
        await died;
        // A hover may be answered without a worker; a compile is not.
        s.edit("main.cpp", { after: 'int x = "s";', insert: "\n" });
        expect((await s.errors("main.cpp")).length, `errors after kill ${kill}`).toBeGreaterThan(0);
        expect(await s.hover(at("main.cpp", "int a|dd("))).not.toBeNull();
    }

    // The file never loses its diagnostics.
    for (const diagnostics of (await s.pushes("main.cpp")).slice(published)) {
        expect(diagnostics.length, "an outage cleared the diagnostics").toBeGreaterThan(0);
    }
    expect(s.workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
});

const NAMES = Array.from({ length: 6 }, (_, i) => `file_${i}.cpp`);

serve.files(Object.fromEntries(NAMES.map((name, i) => [name, `int value_${i} = ${i};\n`])), {
    // One stateful worker holding two documents at most.
    config: { project: { stateful_worker_count: 1 } },
    env: { CLICE_TEST_MAX_DOCUMENTS: "2" },
})("eviction does not loop", async ({ s }) => {
    // A burst three times the cap settles instead of evicting compiles in
    // flight and compiling them again without end, and answers every request.
    for (const name of NAMES) {
        s.open(name);
    }
    const burst = await Promise.all(
        NAMES.map((name) =>
            s.request<proto.SemanticTokens | null>("textDocument/semanticTokens/full", name),
        ),
    );
    for (const tokens of burst) {
        expect(tokens?.data.length).toBeGreaterThan(0);
    }

    // Asked again one by one, the evicted documents come back on demand.
    for (const name of NAMES) {
        expect(await s.hover(at(name, "int v|alue_"))).not.toBeNull();
    }

    // A document compiles once for the burst, once more for each eviction
    // that caught it between a compile and its query, and once when asked
    // again. Each such eviction takes another document landing in that
    // window, so they stay few; a loop of evictions runs far past this.
    const compiles = s.workspace.log("SF-0.log").split("Compile request:").length - 1;
    expect(compiles).toBeLessThanOrEqual(5 * NAMES.length);
});
