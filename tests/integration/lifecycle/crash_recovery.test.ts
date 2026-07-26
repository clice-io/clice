/// Crash-recovery of background indexing.
///
/// Kills stateless workers while an indexing round is in flight and verifies the
/// round still converges: in-flight files fail with worker_crashed, the indexer
/// requeues them, and a follow-up round indexes every file.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { sleep } from "../../tools/checks.ts";
import type { CliceClient } from "../../tools/client.ts";
import { writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

const FILE_COUNT = 20;

function statelessWorkerPids(serverPid: number): number[] {
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
        if (ppid === serverPid && cmdline.includes("SL-")) {
            pids.push(Number(entry));
        }
    }
    return pids;
}

async function indexedFunctions(client: CliceClient): Promise<Set<string>> {
    const result = await client.workspaceSymbols("func_");
    return new Set((result ?? []).map((s) => s.name));
}

test.skipIf(process.platform !== "linux")("crash during indexing", async () => {
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
    try {
        // Enough moderately heavy TUs that the indexing round is still in flight
        // when the workers get killed.
        const files: string[] = [];
        for (let i = 0; i < FILE_COUNT; i++) {
            const name = `file_${i}.cpp`;
            fs.writeFileSync(
                path.join(workspace, name),
                `#include <vector>\n#include <string>\n` +
                    `int func_${i}() { return (int)std::string("${i}").size(); }\n`,
            );
            files.push(name);
        }
        fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
        writeCdb(workspace, [...files, "main.cpp"]);

        // The kills below surface as WorkerCrash anomalies; Debug builds abort on
        // anomalies by design, so disable the trap like anomaly.test does.
        process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
        let client;
        try {
            client = await makeClient(cliceExecutable(), workspace);
        } finally {
            delete process.env["CLICE_ANOMALY_NO_TRAP"];
        }

        try {
            await client.openAndWait(path.join(workspace, "main.cpp"));

            // Wait until the round demonstrably started (first symbols merged),
            // then kill a stateless worker mid-round.
            let killed = false;
            for (let i = 0; i < 300; i++) {
                if ((await indexedFunctions(client)).size > 0) {
                    const workers = statelessWorkerPids(client.child.pid!);
                    if (workers.length > 0) {
                        process.kill(workers[0]!, "SIGKILL");
                        killed = true;
                    }
                    break;
                }
                await sleep(100);
            }
            expect(killed, "indexing never started or no stateless worker found").toBe(true);

            // The files that were in flight on the killed worker must be
            // requeued and indexed by a follow-up round: every function
            // eventually appears in the project index.
            const expected = new Set(Array.from({ length: FILE_COUNT }, (_, i) => `func_${i}`));
            let found = new Set<string>();
            for (let i = 0; i < 120; i++) {
                found = await indexedFunctions(client);
                if ([...expected].every((f) => found.has(f))) {
                    break;
                }
                await sleep(1_000);
            }
            const missing = [...expected].filter((f) => !found.has(f)).sort();
            expect(
                [...expected].every((f) => found.has(f)),
                `missing after crash: ${JSON.stringify(missing)}`,
            ).toBe(true);
        } finally {
            await shutdownClient(client);
        }
    } finally {
        fs.rmSync(workspace, { recursive: true, force: true });
    }
});
