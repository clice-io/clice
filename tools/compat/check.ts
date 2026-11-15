/// One scenario end to end: build the project copy with real tools, then
/// hold clice to what the build proved — every unit parses clean, the
/// macros and the headers found agree with the compiler's, each file's
/// entry reads as written and its command resolves as the scenario expects
/// — and to what the editor answers over that build.

import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { CliceClient } from "../client/client.ts";
import { ending, runProcess } from "../client/process.ts";
import { Workspace } from "../client/workspace.ts";
import { REPO_ROOT } from "../repo.ts";
import { fileCommands, lint } from "./clice.ts";
import {
    entryArguments,
    entrySource,
    nativeEntries,
    readDatabase,
    samePath,
    type DatabaseEntry,
} from "./database.ts";
import { compilerAnswers, writeExpectations } from "./macros.ts";
import {
    buildEnv,
    cliceEnv,
    missingTools,
    STEP_MS,
    type Answer,
    type FileExpectation,
    type Scenario,
} from "./scenario.ts";

const PROJECT_DIR = path.join(REPO_ROOT, "tests", "compat", "project");

async function build(scenario: Scenario, root: string, env: NodeJS.ProcessEnv): Promise<void> {
    for (const [tool, ...args] of scenario.build) {
        const step = await runProcess(tool, args, { cwd: root, env, timeout: STEP_MS });
        if (step.status !== 0) {
            throw new Error(
                `\`${[tool, ...args].join(" ")}\` failed ` +
                    `(${ending(step)})\n${step.stdout}\n${step.stderr}`,
            );
        }
    }
}

function containsSequence(args: string[], sequence: string[]): boolean {
    return args.some((_, start) => sequence.every((arg, i) => args[start + i] === arg));
}

/// The database's entry for `file`, which every checked file has.
function databaseEntry(entries: DatabaseEntry[], root: string, file: string): DatabaseEntry {
    const source = path.join(root, file);
    const entry = entries.find((e) => samePath(entrySource(e), source));
    if (entry === undefined) {
        throw new Error(`the database has no entry for ${file}`);
    }
    return entry;
}

/// The scenario's database as the checks read it.
async function readEntries(scenario: Scenario, root: string): Promise<DatabaseEntry[]> {
    const entries = readDatabase(root);
    return scenario.posix === undefined ? entries : nativeEntries(entries, scenario.posix);
}

/// What is wrong with clice's reading of a file's database entry and the
/// command it resolves it to, empty when nothing is.
async function commandProblems(
    clice: string,
    root: string,
    file: string,
    expectation: FileExpectation,
    entries: DatabaseEntry[],
    env: NodeJS.ProcessEnv,
): Promise<string[]> {
    const [command] = await fileCommands(clice, root, file, env);
    if (command === undefined) {
        throw new Error(`clice command ${file} lists no entry`);
    }
    const written = entryArguments(databaseEntry(entries, root, file));
    const { arguments: resolved } = command.resolved;
    const problems: string[] = [];
    if (written.join("\0") !== command.entry.arguments.join("\0")) {
        problems.push(
            `reads the entry as ${command.entry.arguments.join(" ")}, not ${written.join(" ")}`,
        );
    }
    if (command.toolchainError !== undefined) {
        problems.push(`the compiler query failed: ${command.toolchainError}`);
    }
    for (const sequence of expectation.contains ?? []) {
        const wanted = sequence.map((arg) => arg.replaceAll("${root}", root));
        if (!containsSequence(resolved, wanted)) {
            problems.push(`lacks ${wanted.join(" ")}`);
        }
    }
    for (const arg of expectation.excludes ?? []) {
        if (resolved.includes(arg)) {
            problems.push(`keeps ${arg}`);
        }
    }
    return problems.map((problem) => `${file}: ${problem}\n    ${resolved.join(" ")}`);
}

/// Where `answer` asks in `text`.
function placeOf(text: string, answer: Answer): proto.Position {
    const at = text.indexOf(answer.at);
    if (at < 0) {
        throw new Error(`\`${answer.at}\` is not in the file`);
    }
    const before = text.slice(0, at + (answer.offset ?? 0));
    const line = before.split("\n").length - 1;
    return { line, character: before.length - before.lastIndexOf("\n") - 1 };
}

function definitionPaths(
    answer: proto.Location | proto.Location[] | proto.LocationLink[] | null,
): string[] {
    const locations = answer === null ? [] : Array.isArray(answer) ? answer : [answer];
    return locations.map((location) =>
        URI.parse("targetUri" in location ? location.targetUri : location.uri).fsPath.replaceAll(
            "\\",
            "/",
        ),
    );
}

function hoverText(hover: proto.Hover | null): string {
    const contents = hover?.contents;
    if (contents === undefined) {
        return "";
    }
    const parts = Array.isArray(contents) ? contents : [contents];
    return parts.map((part) => (typeof part === "string" ? part : part.value)).join("\n");
}

/// What is wrong with the editor's answers in the scenario's files, empty
/// when nothing is.
async function answerProblems(
    clice: string,
    ws: Workspace,
    answers: Record<string, Answer[]>,
    env: NodeJS.ProcessEnv,
): Promise<string[]> {
    const problems: string[] = [];
    const client = CliceClient.start(clice, { cwd: ws.root, baseEnv: env });
    try {
        await client.initialize(ws);
        for (const [file, wanted] of Object.entries(answers)) {
            const [uri, text] = await client.openAndWait(file, STEP_MS);
            for (const diagnostic of client.diagnostics.get(client.normalizeUri(uri)) ?? []) {
                if (diagnostic.severity === proto.DiagnosticSeverity.Error) {
                    const message =
                        typeof diagnostic.message === "string"
                            ? diagnostic.message
                            : diagnostic.message.value;
                    problems.push(`${file}:${diagnostic.range.start.line + 1}: ${message}`);
                }
            }
            for (const answer of wanted) {
                const { line, character } = placeOf(text, answer);
                const where = `${file}:${line + 1}:${character + 1} (${answer.at})`;
                const { definition } = answer;
                if (definition !== undefined) {
                    const paths = definitionPaths(await client.definitionAt(uri, line, character));
                    if (!paths.some((found) => found.includes(definition))) {
                        problems.push(
                            `${where}: go to definition answers ${paths.join(", ") || "nothing"}, ` +
                                `no path holding ${definition}`,
                        );
                    }
                }
                if (answer.hover !== undefined) {
                    const card = hoverText(await client.hoverAt(uri, line, character));
                    if (!card.includes(answer.hover)) {
                        problems.push(`${where}: the hover lacks ${answer.hover}: ${card}`);
                    }
                }
                if (answer.completion !== undefined) {
                    const list = await client.completionAt(uri, line, character);
                    const items = list === null ? [] : Array.isArray(list) ? list : list.items;
                    if (!items.some((item) => item.label === answer.completion)) {
                        problems.push(
                            `${where}: completion lacks ${answer.completion}: ` +
                                items
                                    .slice(0, 20)
                                    .map((item) => item.label)
                                    .join(", "),
                        );
                    }
                }
            }
        }
    } finally {
        await client.shutdown();
    }
    return problems;
}

/// Run `body` on a fresh copy of the project, built in the scenario's
/// environment and held to what the checks need from the build: an entry
/// per checked file carrying what `recorded` names, and the compiler's
/// answers written beside each source.
async function withProject(
    scenario: Scenario,
    body: (ws: Workspace, entries: DatabaseEntry[]) => Promise<void>,
): Promise<void> {
    const missing = missingTools(scenario);
    const env = buildEnv(scenario);
    if (missing.length > 0 || env === null) {
        throw new Error(`missing tools: ${missing.join(", ")}`);
    }
    const tmp = Workspace.tmp();
    const ws = new Workspace(tmp.path(scenario.directory ?? "project"));
    try {
        // Node 24's fs.cpSync spells a destination past ASCII wrong on
        // Windows; a rename does not.
        fs.cpSync(PROJECT_DIR, tmp.path("project"), { recursive: true });
        if (scenario.directory !== undefined) {
            fs.renameSync(tmp.path("project"), ws.root);
        }
        await build(scenario, ws.root, env);
        const entries = await readEntries(scenario, ws.root);
        const scratch = ws.path(".compat");
        fs.mkdirSync(scratch);
        for (const [file, expectation] of Object.entries(scenario.files)) {
            const source = ws.path(file);
            const entry = databaseEntry(entries, ws.root, file);
            const recorded = entryArguments(entry);
            for (const sequence of expectation.recorded ?? []) {
                if (!containsSequence(recorded, sequence)) {
                    throw new Error(
                        `${file}: the database entry lacks ${sequence.join(" ")}: ${recorded.join(" ")}`,
                    );
                }
            }
            writeExpectations(source, await compilerAnswers(entry, scratch, env, scenario.posix));
        }
        if (scenario.config !== undefined) {
            ws.write("clice.toml", scenario.config);
        }
        await body(ws, entries);
    } finally {
        tmp.remove();
    }
}

/// Throws with everything clice got wrong about the scenario's build.
export async function checkScenario(clice: string, scenario: Scenario): Promise<void> {
    await withProject(scenario, async (ws, entries) => {
        const env = cliceEnv(scenario);
        const linted = await lint(clice, ws.root, env);
        if (linted.status !== 0 || !linted.report.trimEnd().endsWith(": 0 findings.")) {
            const log = linted.log.split("\n").slice(-30).join("\n");
            throw new Error(`clice lint exited ${linted.status}:\n${linted.report}\n${log}`);
        }

        const problems: string[] = [];
        for (const [file, expectation] of Object.entries(scenario.files)) {
            problems.push(
                ...(await commandProblems(clice, ws.root, file, expectation, entries, env)),
            );
        }
        if (scenario.answers !== undefined) {
            problems.push(...(await answerProblems(clice, ws, scenario.answers, env)));
        }
        if (problems.length > 0) {
            throw new Error(problems.join("\n"));
        }
    });
}
