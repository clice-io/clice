/// End-to-end check of the release's crash symbolization.
///
/// Crashes a worker of the release's stripped clice inside clang and verifies
/// scripts/symbolize.py recovers clice's frames and libclang's from the
/// raw-address crash log against the release's GSYM. This is the guarantee
/// that shipped crash logs stay actionable.

import * as fs from "node:fs";
import * as path from "node:path";
import { runProcess } from "@clice/tools/client";
import { REPO_ROOT } from "@clice/tools/compile-commands";
import { at, cliceExecutable, expect, serve } from "../../fixtures.ts";

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

/// Without in-process symbolization the log carries addresses only, as on
/// a user's machine without llvm-symbolizer.
const ENV = { CLICE_TEST_PRAGMA_CRASH: "1", LLVM_DISABLE_SYMBOLIZATION: "1" };
/// One crash to read: with clang-tidy on, the compile would first retry
/// without it and crash again, and indexing the file would crash another
/// worker.
const CONFIG = { diagnostics: { clang_tidy: false }, project: { enable_indexing: false } };

/// The release's program serves, under its own name, which the crash log's
/// frames carry.
const EXECUTABLE = process.platform === "win32" ? "clice.exe" : "clice";

serve
    .files(
        { "poison.cpp": "int add(int a, int b) { return a + b; }\n#pragma clang __debug crash\n" },
        {
            config: CONFIG,
            env: ENV,
            anomalies: true,
            setup: (workspace) => {
                fs.copyFileSync(release!.stripped, workspace.path(EXECUTABLE));
                fs.chmodSync(workspace.path(EXECUTABLE), 0o755);
            },
            launch: { executable: "${workspace}/" + EXECUTABLE },
        },
    )
    .skipIf(release === undefined)("stripped crash symbolization", async ({ s }) => {
    const { gsym } = release!;

    s.open("poison.cpp", { pull: false });
    expect(await s.hover(at("poison.cpp", "int a|dd("))).toBeNull();
    await s.sync();
    expect(s.workspace.workerCrashes(`compile ${s.workspace.displayPath("poison.cpp")}`)).toBe(1);

    const logsDir = s.workspace.path(".clice/logs");
    const crashLogs = fs
        .readdirSync(logsDir, { recursive: true, encoding: "utf8" })
        .filter((name) => name.endsWith(".log") && path.basename(name) !== "master.log")
        .map((name) => path.join(logsDir, name))
        .filter((p) => fs.readFileSync(p, "utf8").includes("CRASH STACK TRACE"));
    expect(crashLogs.length, "the worker's log should hold its backtrace").toBe(1);
    const crashLog = crashLogs[0] ?? "";
    const raw = fs.readFileSync(crashLog, "utf8");
    expect(raw).toContain("main executable base: 0x");
    expect(raw, "the stripped binary must not symbolize itself").not.toContain("logging.cpp");

    const result = await runProcess(process.platform === "win32" ? "python" : "python3", [
        path.join(REPO_ROOT, "scripts", "symbolize.py"),
        crashLog,
        "--symbols",
        gsym,
    ]);
    expect(result.status, `symbolize.py failed: ${result.stderr.slice(0, 2000)}`).toBe(0);
    const trace = result.stdout.slice(result.stdout.indexOf("CRASH STACK TRACE"));
    // The crash handler is clice's code, the pragma's handler libclang's.
    expect(trace, `clice's frames:\n${trace.slice(0, 6000)}`).toContain("logging.cpp");
    expect(trace, `libclang's frames:\n${trace.slice(0, 6000)}`).toContain("Pragma.cpp");
});
