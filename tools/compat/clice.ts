/// What clice makes of a scenario's build: the batch lint over the whole
/// compilation database (every unit must parse clean, as it compiled clean
/// for the real compiler) and the compile command it resolves per file.

import { spawnSync } from "node:child_process";
import { systemEnv } from "./scenario.ts";

export interface LintRun {
    status: number | null;
    /// The report on stdout: one line per finding, then the summary.
    report: string;
    log: string;
}

export interface CompileCommand {
    file: string;
    directory: string;
    arguments: string[];
    source: string;
    toolchain_error?: string;
}

function runClice(clice: string, args: string[]) {
    return spawnSync(clice, args, {
        env: systemEnv(),
        encoding: "utf8",
        timeout: 300_000,
        maxBuffer: 64 * 1024 * 1024,
    });
}

/// `clice lint --index`: the parse of every unit, and the index the
/// queries below read.
export function lint(clice: string, root: string): LintRun {
    const run = runClice(clice, ["lint", "--workspace", root, "--index"]);
    return { status: run.status, report: run.stdout, log: run.stderr };
}

export function compileCommand(clice: string, root: string, file: string): CompileCommand {
    const run = runClice(clice, [
        "query",
        "--workspace",
        root,
        "--method",
        "compileCommand",
        "--path",
        file,
    ]);
    const answer = JSON.parse(run.stdout) as { result?: CompileCommand; error?: string };
    if (answer.result === undefined) {
        throw new Error(`compileCommand ${file}: ${answer.error ?? run.stderr}`);
    }
    return answer.result;
}

/// Whether `run` occurs in `args` as adjacent arguments.
export function containsRun(args: string[], run: string[]): boolean {
    return args.some((_, start) => run.every((arg, i) => args[start + i] === arg));
}
