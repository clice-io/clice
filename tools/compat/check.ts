/// One scenario end to end: build the project copy with real tools, then
/// hold clice to what the build proved — every unit parses clean, the
/// macros agree with the compiler's, each file's command resolves as the
/// scenario expects.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { Workspace } from "../client/workspace.ts";
import { REPO_ROOT } from "../compile_commands.ts";
import { compileCommand, lint } from "./clice.ts";
import { entryArguments, entrySource, readDatabase } from "./database.ts";
import { compilerMacros, writeExpectations } from "./macros.ts";
import { missingTools, systemEnv, type FileExpectation, type Scenario } from "./scenario.ts";

const PROJECT_DIR = path.join(REPO_ROOT, "tests", "compat", "project");

function build(scenario: Scenario, root: string): void {
    for (const [tool, ...args] of scenario.build) {
        const run = spawnSync(tool, args, {
            cwd: root,
            env: systemEnv(),
            encoding: "utf8",
            maxBuffer: 64 * 1024 * 1024,
        });
        if (run.error !== undefined || run.status !== 0) {
            throw new Error(
                `\`${[tool, ...args].join(" ")}\` failed ` +
                    `(${run.error?.message ?? `exit ${run.status}`})\n${run.stdout}\n${run.stderr}`,
            );
        }
    }
}

function containsSequence(args: string[], sequence: string[]): boolean {
    return args.some((_, start) => sequence.every((arg, i) => args[start + i] === arg));
}

/// What is wrong with a file's resolved command, empty when nothing is.
function commandProblems(
    clice: string,
    root: string,
    file: string,
    expectation: FileExpectation,
): string[] {
    const command = compileCommand(clice, root, file);
    const problems: string[] = [];
    if (command.toolchainError !== null) {
        problems.push(`the compiler query failed: ${command.toolchainError}`);
    }
    if (command.source !== "database") {
        problems.push(`resolved from ${command.source}, not the file's database entry`);
    }
    for (const sequence of expectation.contains ?? []) {
        const wanted = sequence.map((arg) => arg.replaceAll("${root}", root));
        if (!containsSequence(command.arguments, wanted)) {
            problems.push(`lacks ${wanted.join(" ")}`);
        }
    }
    for (const arg of expectation.excludes ?? []) {
        if (command.arguments.includes(arg)) {
            problems.push(`keeps ${arg}`);
        }
    }
    return problems.map((problem) => `${file}: ${problem}\n    ${command.arguments.join(" ")}`);
}

/// Throws with everything clice got wrong about the scenario's build.
export function checkScenario(clice: string, scenario: Scenario): void {
    const missing = missingTools(scenario);
    if (missing.length > 0) {
        throw new Error(`missing tools: ${missing.join(", ")}`);
    }
    const ws = Workspace.tmp();
    try {
        fs.cpSync(PROJECT_DIR, ws.root, { recursive: true });
        build(scenario, ws.root);
        const entries = readDatabase(ws.root);
        const scratch = ws.path(".compat");
        fs.mkdirSync(scratch);
        for (const file of Object.keys(scenario.files)) {
            const source = ws.path(file);
            const entry = entries.find((e) => entrySource(e) === source);
            if (entry === undefined) {
                throw new Error(`the database has no entry for ${file}`);
            }
            const recorded = entryArguments(entry);
            for (const sequence of scenario.files[file]?.recorded ?? []) {
                if (!containsSequence(recorded, sequence)) {
                    throw new Error(
                        `${file}: the database entry lacks ${sequence.join(" ")}: ${recorded.join(" ")}`,
                    );
                }
            }
            writeExpectations(source, compilerMacros(entry, scratch));
        }

        const run = lint(clice, ws.root);
        if (run.status !== 0 || !run.report.endsWith(": 0 findings.\n")) {
            const log = run.log.split("\n").slice(-30).join("\n");
            throw new Error(`clice lint exited ${run.status}:\n${run.report}\n${log}`);
        }

        const problems = Object.entries(scenario.files).flatMap(([file, expectation]) =>
            commandProblems(clice, ws.root, file, expectation),
        );
        if (problems.length > 0) {
            throw new Error(problems.join("\n"));
        }
    } finally {
        ws.remove();
    }
}
