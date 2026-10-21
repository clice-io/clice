/// What clice makes of a scenario's build: the batch lint over the whole
/// compilation database (every unit must parse clean, as it compiled clean
/// for the real compiler) and, per file, `clice command`: the database's
/// entries as clice reads them and the commands it compiles with.

import { run } from "./scenario.ts";

export interface LintRun {
    status: number | null;
    /// The report on stdout: one line per finding, then the summary.
    report: string;
    log: string;
}

export interface CommandLine {
    directory: string;
    arguments: string[];
}

/// One of a file's database entries: as written, its `command` split as
/// clice splits it, and the command clice compiles the file with under it.
export interface EntryCommands {
    database: string;
    entry: CommandLine & { file: string };
    resolved: CommandLine;
    /// Why the compiler could not be queried; `resolved` is then clice's own
    /// reading of the entry.
    toolchainError?: string;
}

/// `clice lint`: the parse of every unit.
export async function lint(clice: string, root: string, env: NodeJS.ProcessEnv): Promise<LintRun> {
    const linted = await run(clice, ["lint", "--workspace", root], { env });
    return { status: linted.status, report: linted.stdout, log: linted.stderr };
}

/// `clice command <file> --json`: the file's entries, in the order the
/// build ranks them.
export async function fileCommands(
    clice: string,
    root: string,
    file: string,
    env: NodeJS.ProcessEnv,
): Promise<EntryCommands[]> {
    const ran = await run(clice, ["command", file, "--workspace", root, "--json"], { env });
    if (ran.status !== 0) {
        throw new Error(
            `clice command ${file}: ${ran.error ?? `exit ${ran.status}`}\n${ran.stdout}\n${ran.stderr}`,
        );
    }
    return (JSON.parse(ran.stdout) as { commands: EntryCommands[] }).commands;
}
