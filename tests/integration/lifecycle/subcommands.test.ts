import { spawnSync } from "node:child_process";
import { cliceExecutable, expect, test } from "../fixtures.ts";

const SUBCOMMANDS = ["serve", "query", "worker", "index", "doc", "lint", "format"];
const STUBS = ["index", "doc", "lint", "format"];

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
