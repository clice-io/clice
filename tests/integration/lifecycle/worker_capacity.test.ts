/// Requests ride out windows without a worker — a restart backoff, an LRU
/// eviction — instead of answering empty or clearing what the file shows.

import * as fs from "node:fs";
import { waitUntil } from "@clice/tools/client";
import { expect, test } from "../fixtures.ts";

function workerPids(serverPid: number, name: string): number[] {
    const pids: number[] = [];
    for (const entry of fs.readdirSync("/proc")) {
        if (!/^\d+$/.test(entry)) {
            continue;
        }
        let stat: string;
        let cmdline: Buffer;
        try {
            stat = fs.readFileSync(`/proc/${entry}/stat`, "utf8");
            cmdline = fs.readFileSync(`/proc/${entry}/cmdline`);
        } catch {
            continue;
        }
        const ppid = Number(
            stat
                .slice(stat.lastIndexOf(")") + 1)
                .trim()
                .split(/\s+/)[1],
        );
        if (ppid === serverPid && cmdline.includes(name)) {
            pids.push(Number(entry));
        }
    }
    return pids;
}

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
                const [pid] = workerPids(client.child.pid!, "SF-");
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
    // flight and compiling them again without end.
    const uris = names.map((name) => client.open(name)[0]);
    await Promise.all(uris.map((uri) => client.semanticTokensFull(uri)));

    // Asked again one by one, the evicted documents come back on demand.
    for (const uri of uris) {
        expect(await client.hoverAt(uri, 0, 5)).not.toBeNull();
    }

    // Every compile answered before its document could be evicted: a
    // document compiles once for the burst, once more if it was evicted
    // between its compile and its query, and once when asked again — never
    // in a loop of evictions.
    const compiles = workspace.log("SF-0.log").split("Compile request:").length - 1;
    expect(compiles).toBeLessThanOrEqual(3 * names.length);
});
