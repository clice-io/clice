import { spawnSync } from "node:child_process";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const SUBCOMMANDS = ["serve", "query", "worker", "index", "doc", "lint", "format"];
const STUBS = ["doc", "lint", "format"];

function runClice(...args: string[]) {
    return spawnSync(cliceExecutable(), args, { encoding: "utf8", timeout: 30_000 });
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
    expect(first.stdout).toContain("Indexed 1 translation units");

    // The second run resumes from the persisted index: the hash gate
    // skips the fresh TU without recompiling it.
    const second = runClice(...args);
    expect(second.status, `stderr: ${second.stderr}`).toBe(0);
    expect(second.stderr).not.toContain("] Indexing ");

    const stats = runClice("index", "--stats", "--workspace", ws.root);
    expect(stats.status, `stderr: ${stats.stderr}`).toBe(0);
    expect(stats.stdout).toContain("Translation units: 1");
});
