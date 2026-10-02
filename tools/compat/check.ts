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
import { entryArguments, entrySource, readDatabase, samePath } from "./database.ts";
import { compilerMacros, writeExpectations } from "./macros.ts";
import { buildEnv, missingTools, type FileExpectation, type Scenario } from "./scenario.ts";

const PROJECT_DIR = path.join(REPO_ROOT, "tests", "compat", "project");

function build(scenario: Scenario, root: string, env: NodeJS.ProcessEnv): void {
    for (const [tool, ...args] of scenario.build) {
        const run = spawnSync(tool, args, {
            cwd: root,
            env,
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

/// Run `body` on a fresh copy of the project, built in the scenario's
/// environment.
function withProject(
    scenario: Scenario,
    body: (ws: Workspace, env: NodeJS.ProcessEnv) => void,
): void {
    const missing = missingTools(scenario);
    const env = buildEnv(scenario);
    if (missing.length > 0 || env === null) {
        throw new Error(`missing tools: ${missing.join(", ")}`);
    }
    const ws = Workspace.tmp();
    try {
        fs.cpSync(PROJECT_DIR, ws.root, { recursive: true });
        build(scenario, ws.root, env);
        body(ws, env);
    } finally {
        ws.remove();
    }
}

/// Hold the build to what the checks need from it: an entry per checked
/// file carrying what `recorded` names, and the compiler's macro values
/// written beside each source.
function prepareChecks(scenario: Scenario, ws: Workspace, env: NodeJS.ProcessEnv): void {
    const entries = readDatabase(ws.root);
    const scratch = ws.path(".compat");
    fs.mkdirSync(scratch);
    for (const [file, expectation] of Object.entries(scenario.files)) {
        const source = ws.path(file);
        const entry = entries.find((e) => samePath(entrySource(e), source));
        if (entry === undefined) {
            throw new Error(`the database has no entry for ${file}`);
        }
        const recorded = entryArguments(entry);
        for (const sequence of expectation.recorded ?? []) {
            if (!containsSequence(recorded, sequence)) {
                throw new Error(
                    `${file}: the database entry lacks ${sequence.join(" ")}: ${recorded.join(" ")}`,
                );
            }
        }
        writeExpectations(source, compilerMacros(entry, scratch, env));
    }
}

/// Throws when the scenario's toolchain cannot build the project and
/// produce what clice would be checked against.
export function checkBuild(scenario: Scenario): void {
    withProject(scenario, (ws, env) => {
        prepareChecks(scenario, ws, env);
    });
}

/// Throws with everything clice got wrong about the scenario's build.
export function checkScenario(clice: string, scenario: Scenario): void {
    withProject(scenario, (ws, env) => {
        prepareChecks(scenario, ws, env);

        const run = lint(clice, ws.root);
        if (run.status !== 0 || !run.report.trimEnd().endsWith(": 0 findings.")) {
            const log = run.log.split("\n").slice(-30).join("\n");
            throw new Error(`clice lint exited ${run.status}:\n${run.report}\n${log}`);
        }

        const problems = Object.entries(scenario.files).flatMap(([file, expectation]) =>
            commandProblems(clice, ws.root, file, expectation),
        );
        if (problems.length > 0) {
            throw new Error(problems.join("\n"));
        }
    });
}
