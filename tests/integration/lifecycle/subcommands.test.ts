import { spawn, type ChildProcess } from "node:child_process";
import { runProcess, waitUntil, type CliceClient } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const SUBCOMMANDS = [
    "serve",
    "query",
    "refactor",
    "worker",
    "index",
    "lint",
    "format",
    "inspect",
    "analyze",
];

function runClice(...args: string[]) {
    return runProcess(cliceExecutable(), args, { timeout: 30_000 });
}

function exitOf(child: ChildProcess): Promise<{ code: number | null; signal: string | null }> {
    return new Promise((resolve) => {
        child.once("close", (code, signal) => {
            resolve({ code, signal });
        });
    });
}

function bigSource(): string {
    return Array.from(
        { length: 1500 },
        (_, i) => `int function_number_${i}(int a, int b) { return a + b; }\n`,
    ).join("");
}

async function waitSymbol(client: CliceClient, name: string): Promise<boolean> {
    return waitUntil(
        async () => {
            const symbols = await client.workspaceSymbols(name);
            return symbols?.some((symbol) => symbol.name === name) ?? false;
        },
        {
            timeout: 30_000,
            interval: 1_000,
            description: `workspace symbol ${name}`,
        },
    );
}

test("root usage lists subcommands", async () => {
    // Both the bare invocation and --help print the root usage and succeed.
    for (const args of [[], ["--help"]]) {
        const result = await runClice(...args);
        expect(result.status).toBe(0);
        for (const name of SUBCOMMANDS) {
            expect(result.stdout).toContain(name);
        }
    }
});

test("subcommand help", async () => {
    for (const name of SUBCOMMANDS) {
        const result = await runClice(name, "--help");
        expect(result.status).toBe(0);
        // analyze groups commands: its help lists them rather than a usage line.
        expect(result.stdout).toContain(name === "analyze" ? "modules" : `clice ${name}`);
    }
});

test("unknown subcommand fails", async () => {
    expect((await runClice("bogus")).status).toBe(2);
});

test("unknown option is a usage error", async () => {
    for (const name of SUBCOMMANDS) {
        expect((await runClice(name, "--bogus")).status, name).toBe(2);
    }
});

test("index subcommand builds and resumes", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    ws.writeCDB(["main.cpp"]);

    // A stats query before any index run reports the missing cache.
    const empty = await runClice("index", "--stats", "--workspace", ws.root);
    expect(empty.status).toBe(1);
    expect(empty.stderr).toContain("No index cache");

    const args = ["index", "--workspace", ws.root, "--workers", "2"];
    const first = await runClice(...args);
    expect(first.status, `stderr: ${first.stderr}`).toBe(0);
    expect(first.stderr).toContain("] Indexing ");
    expect(first.stdout).toContain("Indexed 1 translation unit in");

    // The second run resumes from the persisted index: the hash gate
    // skips the fresh TU without recompiling it.
    const second = await runClice(...args);
    expect(second.status, `stderr: ${second.stderr}`).toBe(0);
    expect(second.stderr).not.toContain("] Indexing ");

    const stats = await runClice("index", "--stats", "--workspace", ws.root);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});

test.skipIf(process.platform === "win32")(
    "repeated SIGTERM saves progress",
    async ({ session }) => {
        const ws = session.tmpdir();
        ws.pinCacheDir();
        const units = Array.from({ length: 24 }, (_, i) => `unit${i}.cpp`);
        for (const [i, unit] of units.entries()) {
            ws.write(unit, `#include <map>\n#include <string>\nint unit${i}() { return ${i}; }\n`);
        }
        ws.writeCDB(units);

        // GNU timeout signals the child, then its whole process group: the
        // second SIGTERM must not turn the graceful stop into an exit that
        // throws the finished units away. One goes out once a unit is indexed,
        // the other once the first was handled.
        const child = spawn(
            cliceExecutable(),
            ["index", "--workspace", ws.root, "--workers", "1"],
            {
                stdio: ["ignore", "pipe", "pipe"],
            },
        );
        const triggers = ["[perf:index] progress=", "Interrupted;"];
        let stdout = "";
        let stderr = "";
        child.stdout.on("data", (chunk: Buffer) => (stdout += chunk.toString()));
        child.stderr.on("data", (chunk: Buffer) => {
            stderr += chunk.toString();
            if (triggers.length > 0 && stderr.includes(triggers[0]!)) {
                triggers.shift();
                child.kill("SIGTERM");
            }
        });
        const { code } = await exitOf(child);
        expect(triggers, `stderr: ${stderr}`).toEqual([]);
        expect(code, `stderr: ${stderr}`).toBe(130);
        expect(stdout).toContain("progress saved");

        const stats = await runClice("index", "--stats", "--workspace", ws.root);
        expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
        expect(stats.stdout).toMatch(/Translation units: [1-9]/);
    },
);

test("index reports header losing host", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("a.h", "#pragma once\ninline int alpha() { return 1; }\n");
    ws.write("main.cpp", '#include "a.h"\nint app_entry() { return alpha(); }\n');
    ws.writeCDB(["main.cpp"]);

    // Standalone-index a.h: edit it on disk while its buffer is open, so the
    // close sees the shard/disk mismatch and reindexes the header with
    // main.cpp as its borrowed host. `beta` can only come from that reindex —
    // the tracker loops are off and main.cpp is never touched again.
    const client = await session.spawn(ws).initialize(ws);
    expect(await waitSymbol(client, "alpha"), "TU never indexed").toBe(true);
    const [headerUri] = await client.openAndWait("a.h");
    ws.write(
        "a.h",
        "#pragma once\ninline int alpha() { return 1; }\ninline int beta() { return 2; }\n",
    );
    client.close(headerUri);
    expect(await waitSymbol(client, "beta"), "header never standalone-indexed").toBe(true);
    await client.shutdown();

    // Offline, the host's command changes and its include of a.h vanishes:
    // reconciliation drops the header's index and no TU can host it any more,
    // so the batch run must report the header as lost coverage.
    ws.write("main.cpp", "int app_entry() { return 0; }\n");
    ws.writeCDB(["main.cpp"], { extraArgs: ["-DHOST_V2"] });

    const second = await runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(second.status, `stderr: ${second.stderr}`).toBe(1);
    expect(second.stderr).toContain("stays uncovered");
    expect(second.stdout).toContain("failed to index");

    // The debt persists across runs: the snapshot keeps recording the
    // dropped header, so every rerun retries it and reports the partial
    // index rather than going silently clean.
    const third = await runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(third.status, `stderr: ${third.stderr}`).toBe(1);
    expect(third.stderr).toContain("stays uncovered");

    // Only deleting the file settles the debt.
    ws.rm("a.h");
    const fourth = await runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(fourth.status, `stderr: ${fourth.stderr}`).toBe(0);
});

test("lint subcommand reports findings", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) {\n    return a / b;\n}\n");
    ws.writeCDB(["main.cpp"]);

    const findings = await runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(findings.status, `stderr: ${findings.stderr}`).toBe(1);
    expect(findings.stdout).toContain("bugprone-integer-division");
    expect(findings.stdout).toContain("main.cpp:2:12");
    expect(findings.stdout).toContain("Linted 1 translation unit in");

    // The clean rewrite is the negative control: same setup, no finding.
    ws.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    const clean = await runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(clean.status, `stderr: ${clean.stderr}`).toBe(0);
    expect(clean.stdout).toContain("0 findings");
});

test("lint applies config extra args", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\nExtraArgs: ["-DRATIO_DIV"]\n');
    // The define exists only through the configuration's ExtraArgs: the
    // finding proves the frozen plan's args reached the compile command.
    ws.write(
        "main.cpp",
        "#ifdef RATIO_DIV\ndouble ratio(int a, int b) { return a / b; }\n#endif\nint main() { return 0; }\n",
    );
    ws.writeCDB(["main.cpp"]);

    const run = await runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("bugprone-integer-division");
});

test("lint with index persists both", async ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) {\n    return a / b;\n}\n");
    ws.writeCDB(["main.cpp"]);

    const run = await runClice("lint", "--index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("bugprone-integer-division");

    // The same parse persisted the index: a stats reader sees the TU.
    const stats = await runClice("index", "--stats", "--workspace", ws.root);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});

test.skipIf(process.platform === "win32")("output survives merged stderr", async ({ session }) => {
    // `2>&1` shares stderr's file description with stdout: only a serving
    // master may switch it to non-blocking.
    const ws = session.tmpdir();
    ws.write("big.cpp", bigSource());
    const inspect = ["inspect", "document_symbol", ws.path("big.cpp"), "--flags", '["-std=c++23"]'];
    const run = await runProcess(
        "sh",
        ["-c", 'exec "$0" "$@" 2>&1', cliceExecutable(), ...inspect],
        {
            timeout: 60_000,
        },
    );
    expect(run.status, run.stdout.slice(-2000)).toBe(0);
    expect(run.stdout).toContain("function_number_1499");
});

test.skipIf(process.platform === "win32")("closed reader ends quietly", async ({ session }) => {
    const ws = session.tmpdir();
    ws.write("big.cpp", bigSource());
    const child = spawn(
        cliceExecutable(),
        ["inspect", "document_symbol", ws.path("big.cpp"), "--flags", '["-std=c++23"]'],
        { stdio: ["ignore", "pipe", "pipe"] },
    );
    let stderr = "";
    child.stderr.on("data", (chunk: Buffer) => (stderr += chunk.toString()));
    child.stdout.once("data", () => child.stdout.destroy());
    expect(await exitOf(child), `stderr: ${stderr}`).toEqual({ code: null, signal: "SIGPIPE" });
});
