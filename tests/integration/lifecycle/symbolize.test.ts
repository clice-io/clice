/// End-to-end check of the release's crash symbolization.
///
/// Crashes a worker of the release's stripped clice inside clang and verifies
/// scripts/symbolize.py recovers clice's frames and libclang's from the
/// raw-address crash log against the release's GSYM. This is the guarantee
/// that shipped crash logs stay actionable.

import * as fs from "node:fs";
import * as path from "node:path";
import { runProcess, waitUntil } from "@clice/tools/client";
import { REPO_ROOT } from "@clice/tools/compile-commands";
import { cliceExecutable, expect, test } from "../fixtures.ts";

/// `//:package`'s clice and `//:symbols`' GSYM, next to the programs.
function releaseFiles(): { stripped: string; gsym: string } | undefined {
    let bin: string;
    try {
        bin = path.dirname(cliceExecutable());
    } catch {
        return undefined;
    }
    if (bin.split(path.sep).includes("Debug")) {
        return undefined;
    }
    const files = {
        stripped: path.join(bin, "clice.stripped"),
        gsym: path.join(path.dirname(bin), "clice.gsym"),
    };
    // CI builds them on every RelWithDebInfo leg; a local build has them
    // after `pixi run build RelWithDebInfo -- //:package //:symbols`.
    return process.env["CI"] !== undefined || fs.existsSync(files.gsym) ? files : undefined;
}

const release = releaseFiles();

test.skipIf(release === undefined)(
    "stripped crash symbolization",
    async ({ session }) => {
        const { stripped, gsym } = release!;

        // Under the program's own name, which the crash log's frames carry.
        const workspace = session.tmpdir();
        const executable = workspace.path(process.platform === "win32" ? "clice.exe" : "clice");
        fs.copyFileSync(stripped, executable);
        fs.chmodSync(executable, 0o755);

        workspace.write(
            "poison.cpp",
            "int add(int a, int b) { return a + b; }\n#pragma clang __debug crash\n",
        );
        workspace.writeCDB(["poison.cpp"]);
        // Without in-process symbolization the log carries addresses only,
        // as on a user's machine without llvm-symbolizer.
        const client = session.spawn(workspace, {
            executable,
            allowAnomaly: true,
            env: {
                CLICE_ANOMALY_NO_TRAP: "1",
                CLICE_TEST_PRAGMA_CRASH: "1",
                LLVM_DISABLE_SYMBOLIZATION: "1",
            },
        });
        await client.initialize(workspace);
        const compile = `compile ${workspace.displayPath("poison.cpp")}`;
        const [uri] = client.open("poison.cpp");
        expect(await client.hoverAt(uri, 0, 5)).toBeNull();
        await waitUntil(() => workspace.workerCrashes(compile) >= 1, {
            timeout: 20_000,
            interval: 200,
            description: "the worker crash",
        });
        expect(workspace.workerCrashes(compile)).toBe(1);

        const logsDir = workspace.path(".clice/logs");
        const crashLogs = fs
            .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
            .filter((name) => name.endsWith(".log") && path.basename(name) !== "master.log")
            .map((name) => path.join(logsDir, name))
            .filter((p) => fs.readFileSync(p, "utf8").includes("CRASH STACK TRACE"));
        expect(crashLogs.length, "the worker's log should hold its backtrace").toBe(1);
        const raw = fs.readFileSync(crashLogs[0]!, "utf8");
        expect(raw).toContain("main executable base: 0x");
        expect(raw, "the stripped binary must not symbolize itself").not.toContain("logging.cpp");

        const result = await runProcess(process.platform === "win32" ? "python" : "python3", [
            path.join(REPO_ROOT, "scripts", "symbolize.py"),
            crashLogs[0]!,
            "--symbols",
            gsym,
        ]);
        expect(result.status, `symbolize.py failed: ${result.stderr.slice(0, 2000)}`).toBe(0);
        const frames = result.stdout.slice(0, 6000);
        // The crash handler is clice's code, the pragma's handler libclang's.
        expect(frames, `clice's frames:\n${frames}`).toContain("logging.cpp");
        expect(frames, `libclang's frames:\n${frames}`).toContain("Pragma.cpp");
    },
    120_000,
);
