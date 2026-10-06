/// Requests ride out windows without a worker — a restart backoff, an LRU
/// eviction — instead of answering empty or clearing what the file shows.

import { waitUntil } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

test.skipIf(process.platform !== "linux")(
    "outage keeps diagnostics",
    { timeout: 120_000 },
    async ({ session }) => {
        const workspace = session.tmpdir();
        workspace.write("main.cpp", 'int add(int a, int b) { return a + b; }\nint x = "s";\n');
        workspace.writeCDB(["main.cpp"]);
        const client = session.spawn(workspace, {
            allowAnomaly: true,
            env: { CLICE_ANOMALY_NO_TRAP: "1" },
        });
        await client.initialize(workspace, {
            initializationOptions: { project: { stateful_worker_count: 1 } },
        });
        const [uri] = await client.openAndWait("main.cpp");
        client.assertHasErrors(uri);
        const published = client.publishedDiagnostics.length;

        // Fast kills push the lone stateful worker into its respawn backoff.
        let kills = 0;
        let previous = 0;
        await waitUntil(
            () => {
                const [pid] = client.workerPids("SF-");
                if (pid !== undefined && pid !== previous) {
                    process.kill(pid, "SIGKILL");
                    previous = pid;
                    kills += 1;
                }
                return kills === 3;
            },
            { timeout: 30_000, interval: 50, description: "three stateful worker kills" },
        );

        // The request waits for the worker to come back instead of
        // answering empty, and the file never loses its diagnostics.
        expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
        for (const params of client.publishedDiagnostics.slice(published)) {
            if (client.normalizeUri(params.uri) === uri) {
                expect(
                    params.diagnostics.length,
                    "an outage cleared the diagnostics",
                ).toBeGreaterThan(0);
            }
        }
        expect(workspace.log("master.log")).toContain("[anomaly:WorkerCrash]");
    },
);

test("eviction does not loop", async ({ session }) => {
    const workspace = session.tmpdir();
    const names: string[] = [];
    for (let i = 0; i < 6; i++) {
        const name = `file_${i}.cpp`;
        workspace.write(name, `int value_${i} = ${i};\n`);
        names.push(name);
    }
    workspace.writeCDB(names);
    // One stateful worker holding two documents at most.
    const client = session.spawn(workspace, { env: { CLICE_TEST_MAX_DOCUMENTS: "2" } });
    await client.initialize(workspace, {
        initializationOptions: { project: { stateful_worker_count: 1 } },
    });

    // A burst three times the cap settles instead of evicting compiles in
    // flight and compiling them again without end, and answers every request.
    const uris = names.map((name) => client.open(name)[0]);
    const burst = await Promise.all(uris.map((uri) => client.semanticTokensFull(uri)));
    for (const tokens of burst) {
        expect(tokens?.data.length).toBeGreaterThan(0);
    }

    // Asked again one by one, the evicted documents come back on demand.
    for (const uri of uris) {
        expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
    }

    // A document compiles once for the burst, once more for each eviction
    // that caught it between a compile and its query, and once when asked
    // again. Each such eviction takes another document landing in that
    // window, so they stay few; a loop of evictions runs far past this.
    const compiles = workspace.log("SF-0.log").split("Compile request:").length - 1;
    expect(compiles).toBeLessThanOrEqual(5 * names.length);
});
