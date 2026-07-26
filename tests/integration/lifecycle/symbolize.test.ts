/// End-to-end check of the release symbol-separation flow.
///
/// Replays the release pipeline on the freshly built binary (split DWARF, convert
/// to GSYM, strip), crashes a worker of the stripped binary, and verifies
/// scripts/symbolize.py recovers function/file information from the raw-address
/// crash log. This is the guarantee that shipped crash logs stay actionable.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { anomaliesInLogMessages, sleep } from "../../tools/checks.ts";
import { REPO_ROOT, writeCdb } from "../../tools/compile_commands.ts";
import { cliceExecutable, expect, test } from "../../tools/fixtures.ts";
import { makeClient, shutdownClient } from "../../tools/lifecycle.ts";

function childPids(parentPid: number): number[] {
    const pids: number[] = [];
    for (const entry of fs.readdirSync("/proc")) {
        if (!/^\d+$/.test(entry)) {
            continue;
        }
        let stat: string;
        try {
            stat = fs.readFileSync(`/proc/${entry}/stat`, "utf8");
        } catch {
            continue;
        }
        const ppid = Number(
            stat
                .slice(stat.lastIndexOf(")") + 1)
                .trim()
                .split(/\s+/)[1],
        );
        if (ppid === parentPid) {
            pids.push(Number(entry));
        }
    }
    return pids;
}

function which(tool: string): boolean {
    return (process.env["PATH"] ?? "").split(path.delimiter).some((dir) => {
        try {
            fs.accessSync(path.join(dir, tool), fs.constants.X_OK);
            return true;
        } catch {
            return false;
        }
    });
}

// llvm-gsymutil floods stdout with its conversion log; match Python's
// subprocess.run (unbounded capture) so a large log never overflows the
// default 1 MB buffer and kills the child.
const SPAWN_OPTS = { encoding: "utf8", maxBuffer: 512 * 1024 * 1024 } as const;

function runTool(...command: string[]): void {
    const result = spawnSync(command[0]!, command.slice(1), SPAWN_OPTS);
    expect(result.status, `${command[0]} failed: ${result.stderr.slice(0, 2000)}`).toBe(0);
}

function isDebugBuild(): boolean {
    try {
        return cliceExecutable().split(path.sep).includes("Debug");
    } catch {
        return false;
    }
}

test.skipIf(process.platform !== "linux" || isDebugBuild())(
    "stripped crash symbolization",
    async () => {
        const executable = cliceExecutable();

        for (const tool of ["llvm-objcopy", "llvm-strip", "llvm-gsymutil"]) {
            expect(which(tool), `${tool} must be available for the symbol flow`).toBe(true);
        }

        const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "clice-test-"));
        try {
            // Replay the clice-strip / clice-pack-symbol steps from cmake/release.cmake.
            const stripped = path.join(tmp, "clice");
            const debugFile = path.join(tmp, "clice.debug");
            const gsymFile = path.join(tmp, "clice.gsym");
            fs.copyFileSync(executable, stripped);
            fs.chmodSync(stripped, 0o755);
            runTool("llvm-objcopy", "--only-keep-debug", stripped, debugFile);
            runTool(
                "llvm-gsymutil",
                "--convert",
                debugFile,
                "--merged-functions",
                "--quiet",
                "--out-file",
                gsymFile,
            );
            runTool("llvm-strip", "--strip-debug", "--strip-unneeded", stripped);

            const workspace = path.join(tmp, "ws");
            fs.mkdirSync(workspace);
            fs.writeFileSync(path.join(workspace, "main.cpp"), "int main() { return 0; }\n");
            writeCdb(workspace, ["main.cpp"]);

            // Force the raw-address dump: with in-process symbolization disabled the
            // log carries only "clice 0x..." frames, exactly like a user machine
            // without llvm-symbolizer.
            process.env["LLVM_DISABLE_SYMBOLIZATION"] = "1";
            process.env["CLICE_ANOMALY_NO_TRAP"] = "1";
            let client;
            try {
                client = await makeClient(stripped, workspace);
            } finally {
                delete process.env["LLVM_DISABLE_SYMBOLIZATION"];
                delete process.env["CLICE_ANOMALY_NO_TRAP"];
            }

            try {
                await client.openAndWait(path.join(workspace, "main.cpp"));

                const workers = childPids(client.child.pid!);
                expect(
                    workers.length,
                    "server should have spawned worker processes",
                ).toBeGreaterThan(0);
                // SIGABRT for the same reasons as anomaly.test: it exercises the crash
                // handler and is not intercepted by sanitizers.
                process.kill(workers[0]!, "SIGABRT");

                for (let i = 0; i < 50; i++) {
                    if (anomaliesInLogMessages(client).includes("WorkerCrash")) {
                        break;
                    }
                    await sleep(200);
                }
            } finally {
                await shutdownClient(client);
            }

            const logsDir = path.join(workspace, ".clice", "logs");
            const crashLogs = fs
                .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
                .filter((name) => name.endsWith(".log") && path.basename(name) !== "master.log")
                .map((name) => path.join(logsDir, name))
                .filter((p) => fs.readFileSync(p, "utf8").includes("CRASH STACK TRACE"));
            expect(
                crashLogs.length,
                "worker crash backtrace should be written to its log file",
            ).toBeGreaterThan(0);
            const raw = fs.readFileSync(crashLogs[0]!, "utf8");
            expect(raw).toContain("main executable base: 0x");
            expect(raw, "stripped binary must not self-symbolize").not.toContain("logging.cpp");

            const result = spawnSync(
                "python3",
                [
                    path.join(REPO_ROOT, "scripts", "symbolize.py"),
                    crashLogs[0]!,
                    "--symbols",
                    gsymFile,
                ],
                SPAWN_OPTS,
            );
            expect(result.status, `symbolize.py failed: ${result.stderr.slice(0, 2000)}`).toBe(0);
            // The crash handler itself is always on the stack; recovering its source
            // file proves rebasing and GSYM lookup both worked.
            expect(
                result.stdout,
                `symbolized output should name the crash handler source:\n${result.stdout.slice(
                    0,
                    3000,
                )}`,
            ).toContain("logging.cpp");
        } finally {
            fs.rmSync(tmp, { recursive: true, force: true });
        }
    },
    600_000,
);
