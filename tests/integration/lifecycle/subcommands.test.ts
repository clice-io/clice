import { spawnSync } from "node:child_process";
import { waitUntil, type CliceClient } from "@clice/tools/client";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const SUBCOMMANDS = ["serve", "query", "worker", "index", "doc", "lint", "format"];
const STUBS = ["doc", "format"];

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, { encoding: "utf8", timeout: 30_000 });
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

test("root usage lists subcommands", () => {
    // Both the bare invocation and --help print the root usage and succeed.
    for (const args of [[], ["--help"]]) {
        const result = runClice(...args);
        expect(result.status).toBe(0);
        for (const name of SUBCOMMANDS) {
            expect(result.stdout).toContain(name);
        }
    }
});

test("stubs report unimplemented", () => {
    // Stubs explain themselves on stderr and exit non-zero: the command is
    // still unavailable and scripts must be able to detect that.
    for (const name of STUBS) {
        const result = runClice(name);
        expect(result.status).toBe(1);
        expect(result.stderr).toContain("not implemented");
    }
});

test("subcommand help", () => {
    for (const name of SUBCOMMANDS) {
        const result = runClice(name, "--help");
        expect(result.status).toBe(0);
        expect(result.stdout).toContain(`clice ${name}`);
    }
});

test("unknown subcommand fails", () => {
    expect(runClice("bogus").status).not.toBe(0);
});

test("index subcommand builds and resumes", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    ws.writeCDB(["main.cpp"]);

    // A stats query before any index run reports the missing cache.
    const empty = runClice("index", "--stats", "--workspace", ws.root);
    expect(empty.status).toBe(1);
    expect(empty.stderr).toContain("No index cache");

    const args = ["index", "--workspace", ws.root, "--workers", "2"];
    const first = runClice(...args);
    expect(first.status, `stderr: ${first.stderr}`).toBe(0);
    expect(first.stderr).toContain("] Indexing ");
    expect(first.stdout).toContain("Indexed 1 translation unit in");

    // The second run resumes from the persisted index: the hash gate
    // skips the fresh TU without recompiling it.
    const second = runClice(...args);
    expect(second.status, `stderr: ${second.stderr}`).toBe(0);
    expect(second.stderr).not.toContain("] Indexing ");

    const stats = runClice("index", "--stats", "--workspace", ws.root);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});

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

    const second = runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(second.status, `stderr: ${second.stderr}`).toBe(1);
    expect(second.stderr).toContain("stays uncovered");
    expect(second.stdout).toContain("failed to index");

    // The debt persists across runs: the snapshot keeps recording the
    // dropped header, so every rerun retries it and reports the partial
    // index rather than going silently clean.
    const third = runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(third.status, `stderr: ${third.stderr}`).toBe(1);
    expect(third.stderr).toContain("stays uncovered");

    // Only deleting the file settles the debt.
    ws.rm("a.h");
    const fourth = runClice("index", "--workspace", ws.root, "--workers", "2");
    expect(fourth.status, `stderr: ${fourth.stderr}`).toBe(0);
});

test("lint subcommand reports findings", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) {\n    return a / b;\n}\n");
    ws.writeCDB(["main.cpp"]);

    const findings = runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(findings.status, `stderr: ${findings.stderr}`).toBe(1);
    expect(findings.stdout).toContain("bugprone-integer-division");
    expect(findings.stdout).toContain("main.cpp:2:12");
    expect(findings.stdout).toContain("Linted 1 translation unit in");

    // The clean rewrite is the negative control: same setup, no finding.
    ws.write("main.cpp", "int add(int a, int b) { return a + b; }\n");
    const clean = runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(clean.status, `stderr: ${clean.stderr}`).toBe(0);
    expect(clean.stdout).toContain("0 findings");
});

test("lint applies config extra args", ({ session }) => {
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

    const run = runClice("lint", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("bugprone-integer-division");
});

test("lint with index persists both", ({ session }) => {
    const ws = session.tmpdir();
    ws.pinCacheDir();
    ws.write(".clang-tidy", 'Checks: "-*,bugprone-integer-division"\n');
    ws.write("main.cpp", "double ratio(int a, int b) {\n    return a / b;\n}\n");
    ws.writeCDB(["main.cpp"]);

    const run = runClice("lint", "--index", "--workspace", ws.root, "--workers", "2");
    expect(run.status, `stderr: ${run.stderr}`).toBe(1);
    expect(run.stdout).toContain("bugprone-integer-division");

    // The same parse persisted the index: a stats reader sees the TU.
    const stats = runClice("index", "--stats", "--workspace", ws.root);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});
