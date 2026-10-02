/// What clice makes of a scenario's build: the batch lint over the whole
/// compilation database (every unit must parse clean, as it compiled clean
/// for the real compiler) and the compile command it resolves per file.

import { run, systemEnv } from "./scenario.ts";

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
    toolchainError: string | null;
}

/// `clice lint --index`: the parse of every unit, and the index the
/// queries below read.
export async function lint(clice: string, root: string): Promise<LintRun> {
    const linted = await run(clice, ["lint", "--workspace", root, "--index"], { env: systemEnv() });
    return { status: linted.status, report: linted.stdout, log: linted.stderr };
}

export async function compileCommand(
    clice: string,
    root: string,
    file: string,
): Promise<CompileCommand> {
    const query = await run(
        clice,
        ["query", "--workspace", root, "--method", "compileCommand", "--path", file],
        { env: systemEnv() },
    );
    if (query.status !== 0) {
        throw new Error(
            `compileCommand ${file}: ${query.error ?? `exit ${query.status}`}\n${query.stdout}\n${query.stderr}`,
        );
    }
    const answer = JSON.parse(query.stdout) as { result?: CompileCommand; error?: string };
    if (answer.result === undefined) {
        throw new Error(`compileCommand ${file}: ${answer.error ?? query.stderr}`);
    }
    return answer.result;
}
