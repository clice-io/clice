/// The diagnostics of a real project's files, as four tools report them:
/// clang itself under each file's compile command, clice in the editor,
/// clangd, and `clice lint`. Only what a tool reports in the file itself
/// is compared; the report lists, by kind of diagnostic, what one tool
/// says and another does not, or says on another line.
///
/// Usage:
///   node tools/compat/diagnostics.ts --workspace <dir> [options]
///
/// Options:
///   --files <regex>     only the database's files matching it
///   --limit <N>         at most N files, spread evenly over the matches
///   --clice <path>      clice (default: build/RelWithDebInfo/bin/bin/clice)
///   --clang <path>      the compiler to check against (default: clang++)
///   --clangd <path>     clangd, or "none" to leave it out (default: clangd)
///   --no-lint           leave `clice lint` out
///   --jobs <N>          files open in a server at once (default 4)
///   --json <path>       also write every file's diagnostics as JSON
///
/// The workspace's compile_commands.json is at its root or in build/; its
/// clice.toml and .clang-tidy apply to clice and clangd alike.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import { entryArguments, entrySource, readDatabase, type DatabaseEntry } from "./database.ts";
import { run, systemEnv } from "./scenario.ts";

const TOOLS = ["clang", "clice", "clangd", "lint"] as const;
type Tool = (typeof TOOLS)[number];

/// One diagnostic in the file it is compared on.
interface Finding {
    /// 1-based.
    line: number;
    severity: "error" | "warning" | "info";
    message: string;
    /// The compiler's warning flag, the clang-tidy check or clice's code:
    /// whatever the tool names it by.
    name: string;
    /// A clang-tidy finding rather than the compiler's.
    tidy: boolean;
}

interface Options {
    workspace: string;
    files: RegExp | null;
    limit: number | null;
    clice: string;
    clang: string;
    clangd: string | null;
    lint: boolean;
    jobs: number;
    json: string | null;
}

function fail(message: string): never {
    process.stderr.write(`${message}\n`);
    process.exit(2);
}

function options(): Options {
    const { values } = parseArgs({
        options: {
            workspace: { type: "string" },
            files: { type: "string" },
            limit: { type: "string" },
            clice: { type: "string" },
            clang: { type: "string" },
            clangd: { type: "string" },
            "no-lint": { type: "boolean" },
            jobs: { type: "string" },
            json: { type: "string" },
        },
    });
    if (values.workspace === undefined) {
        fail("--workspace is required");
    }
    return {
        workspace: path.resolve(values.workspace),
        files: values.files === undefined ? null : new RegExp(values.files),
        limit: values.limit === undefined ? null : Number(values.limit),
        clice: values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        clang: values.clang ?? "clang++",
        clangd: values.clangd === "none" ? null : (values.clangd ?? "clangd"),
        lint: values["no-lint"] !== true,
        jobs: Number(values.jobs ?? "4"),
        json: values.json ?? null,
    };
}

/// The database's entries under check: one per file, `--limit` of them
/// taken at even steps so a sample covers the whole tree.
function selectEntries(opts: Options): DatabaseEntry[] {
    const seen = new Set<string>();
    const entries = readDatabase(opts.workspace).filter((entry) => {
        const file = entrySource(entry);
        if (seen.has(file) || (opts.files !== null && !opts.files.test(file))) {
            return false;
        }
        seen.add(file);
        return true;
    });
    if (opts.limit === null || entries.length <= opts.limit) {
        return entries;
    }
    const step = entries.length / opts.limit;
    return Array.from({ length: opts.limit }, (_, i) => entries[Math.floor(i * step)]).filter(
        (entry): entry is DatabaseEntry => entry !== undefined,
    );
}

/// A message as every tool can agree on it: clangd capitalizes the first
/// letter and says when a fix is attached.
function normalize(message: string): string {
    const text = (message.split("\n")[0] ?? "").replace(/ \(fix available\)$/, "");
    return text.charAt(0).toLowerCase() + text.slice(1);
}

function severity(level: string): Finding["severity"] {
    if (level === "error" || level === "fatal error") {
        return "error";
    }
    return level === "warning" ? "warning" : "info";
}

/// The arguments that make the compiler only check the file: no object,
/// no dependency file.
function checkArguments(args: string[]): string[] {
    const out: string[] = [];
    for (let i = 1; i < args.length; i += 1) {
        const arg = args[i] ?? "";
        if (["-o", "-MF", "-MT", "-MQ"].includes(arg)) {
            i += 1;
            continue;
        }
        if (["-c", "-MD", "-MMD"].includes(arg) || arg.startsWith("-o")) {
            continue;
        }
        out.push(arg);
    }
    return [...out, "-fsyntax-only", "-fno-color-diagnostics", "-fno-caret-diagnostics"];
}

const COMPILER_LINE =
    /^(.*?):(\d+):(\d+): (error|fatal error|warning|remark|note): (.*?)(?: \[([^\]]+)\])?$/;

/// What clang reports in the file, or why it did not check all of it: the
/// command does not run (a module file the build has not left on disk), or
/// a fatal error stops it.
async function clangFindings(opts: Options, entry: DatabaseEntry): Promise<Finding[] | string> {
    const file = entrySource(entry);
    const ran = await run(opts.clang, checkArguments(entryArguments(entry)), {
        cwd: entry.directory,
        env: systemEnv(),
    });
    if (ran.status === null) {
        return ran.error ?? "clang did not run";
    }
    const findings: Finding[] = [];
    for (const text of ran.stderr.split("\n")) {
        const match = COMPILER_LINE.exec(text);
        if (match === null) {
            if (text.includes("error: ")) {
                return text;
            }
            continue;
        }
        const [, where = "", line = "0", , level = "", message = "", flags = ""] = match;
        if (level === "fatal error") {
            return text;
        }
        if (level === "note" || path.resolve(entry.directory, where) !== file) {
            continue;
        }
        findings.push({
            line: Number(line),
            severity: severity(level),
            message: normalize(message),
            name: flags.split(",")[0] ?? "",
            tidy: false,
        });
    }
    return findings;
}

function fromLSP(diagnostic: proto.Diagnostic): Finding {
    const level = diagnostic.severity ?? proto.DiagnosticSeverity.Error;
    const message = typeof diagnostic.message === "string" ? diagnostic.message : "";
    return {
        line: diagnostic.range.start.line + 1,
        severity:
            level === proto.DiagnosticSeverity.Error
                ? "error"
                : level === proto.DiagnosticSeverity.Warning
                  ? "warning"
                  : "info",
        message: normalize(message),
        name: diagnostic.code === undefined ? "" : String(diagnostic.code),
        tidy: diagnostic.source === "clang-tidy",
    };
}

/// Each file opened alone, `jobs` at a time, and its diagnostics taken
/// once the server answers them for the text as opened.
async function serverFindings(
    tool: "clice" | "clangd",
    opts: Options,
    entries: DatabaseEntry[],
): Promise<Map<string, Finding[]>> {
    const directory = path.dirname(
        ["compile_commands.json", "build/compile_commands.json"]
            .map((rel) => path.join(opts.workspace, rel))
            .find((file) => fs.existsSync(file)) ?? opts.workspace,
    );
    const client = CliceClient.start(tool === "clice" ? opts.clice : (opts.clangd ?? ""), {
        args: tool === "clice" ? ["serve"] : [`--compile-commands-dir=${directory}`],
        cwd: tool === "clangd" ? directory : undefined,
    });
    await client.initialize(new Workspace(opts.workspace), {
        testDefaults: false,
        capabilities: {
            textDocument: {
                publishDiagnostics: { relatedInformation: true },
                ...(tool === "clice" ? { diagnostic: {} } : {}),
            },
        },
    });
    const result = new Map<string, Finding[]>();
    const queue = [...entries];
    const worker = async () => {
        for (let entry = queue.shift(); entry !== undefined; entry = queue.shift()) {
            const file = entrySource(entry);
            const [uri] = client.open(file);
            let diagnostics: proto.Diagnostic[] = [];
            try {
                if (tool === "clice") {
                    diagnostics = await withTimeout(client.pullDiagnostics(uri), 300_000, file);
                } else {
                    const arrived = client.armDiagnostics(uri);
                    await withTimeout(arrived, 300_000, file);
                    diagnostics = client.diagnostics.get(uri) ?? [];
                }
            } catch (error) {
                process.stderr.write(`${tool}: ${file}: ${String(error)}\n`);
            }
            client.close(uri);
            result.set(file, diagnostics.map(fromLSP));
            process.stderr.write(`${tool}: ${result.size}/${entries.length}\r`);
        }
    };
    await Promise.all(Array.from({ length: opts.jobs }, worker));
    process.stderr.write("\n");
    if (tool === "clice") {
        await client.shutdown();
    } else {
        await client.sendRequest("shutdown");
        await client.sendNotification("exit");
        client.disposed = true;
        // Hang detection: a clangd still running after `exit` is killed.
        setTimeout(() => {
            client.child.kill("SIGKILL");
        }, 5_000).unref();
        await client.exited;
    }
    return result;
}

const LINT_LINE = /^(.*?):(\d+):(\d+): (error|warning): (.*) \[([^\]]+)\]$/;

/// `clice lint` over the whole workspace, its findings by file.
async function lintFindings(opts: Options): Promise<Map<string, Finding[]>> {
    const linted = await run(opts.clice, ["lint", "--workspace", opts.workspace], {
        env: systemEnv(),
    });
    const result = new Map<string, Finding[]>();
    for (const text of linted.stdout.split("\n")) {
        const match = LINT_LINE.exec(text);
        if (match === null) {
            continue;
        }
        const [, file = "", line = "0", , level = "", message = "", check = ""] = match;
        const compiler = check.startsWith("clang-diagnostic-");
        const findings = result.get(file) ?? [];
        findings.push({
            line: Number(line),
            severity: severity(level),
            message: normalize(message),
            name: compiler ? `-W${check.slice("clang-diagnostic-".length)}` : check,
            tidy: !compiler,
        });
        result.set(file, findings);
    }
    return result;
}

/// A message with what it names left out, so one kind of diagnostic
/// groups across the places it is raised.
function shape(finding: Finding): string {
    return `${finding.tidy ? "tidy" : "compiler"} ${finding.severity}: ${finding.message
        .replace(/'[^']*'/g, "'…'")
        .replace(/"[^"]*"/g, '"…"')
        .replace(/\d+/g, "N")}`;
}

interface Difference {
    /// Which tools report it, and on which line.
    lines: Partial<Record<Tool, number>>;
    file: string;
    finding: Finding;
}

/// The diagnostics of one file the tools disagree on: each message, with
/// the lines the tools that report it put it on, kept when one of the
/// tools compared on this kind of diagnostic misses it or puts it
/// elsewhere. A message raised several times agrees where every tool puts
/// one on the same line; the occurrences left pair up in line order. The
/// compiler's diagnostics are compared across all four, clang-tidy's
/// between clice, clangd and lint.
function differences(file: string, by: Partial<Record<Tool, Finding[]>>): Difference[] {
    const merged = new Map<string, { finding: Finding; lines: Partial<Record<Tool, number[]>> }>();
    for (const tool of TOOLS) {
        for (const finding of by[tool] ?? []) {
            const key = `${finding.tidy}|${finding.message}`;
            const entry = merged.get(key) ?? { finding, lines: {} };
            (entry.lines[tool] ??= []).push(finding.line);
            merged.set(key, entry);
        }
    }
    const result: Difference[] = [];
    for (const { finding, lines } of merged.values()) {
        const tools = TOOLS.filter(
            (tool) => by[tool] !== undefined && !(finding.tidy && tool === "clang"),
        );
        const left = new Map(tools.map((tool) => [tool, [...(lines[tool] ?? [])]]));
        for (const line of new Set(tools.flatMap((tool) => lines[tool] ?? []))) {
            while (tools.every((tool) => left.get(tool)?.includes(line))) {
                for (const list of left.values()) {
                    list.splice(list.indexOf(line), 1);
                }
            }
        }
        for (const list of left.values()) {
            list.sort((a, b) => a - b);
        }
        const count = Math.max(...[...left.values()].map((list) => list.length));
        for (let i = 0; i < count; i += 1) {
            const occurrence: Partial<Record<Tool, number>> = {};
            for (const [tool, list] of left) {
                const line = list[i];
                if (line !== undefined) {
                    occurrence[tool] = line;
                }
            }
            result.push({ lines: occurrence, file, finding });
        }
    }
    return result;
}

function report(
    root: string,
    entries: DatabaseEntry[],
    found: Partial<Record<Tool, Map<string, Finding[]>>>,
    unchecked: Map<string, string>,
) {
    const all: Difference[] = [];
    for (const entry of entries) {
        const file = entrySource(entry);
        const by: Partial<Record<Tool, Finding[]>> = {};
        for (const tool of TOOLS) {
            const map = found[tool];
            if (map !== undefined && !(tool === "clang" && unchecked.has(file))) {
                by[tool] = map.get(file) ?? [];
            }
        }
        all.push(...differences(file, by));
    }
    const groups = new Map<string, Difference[]>();
    for (const difference of all) {
        const reported = TOOLS.filter((tool) => difference.lines[tool] !== undefined).join("+");
        const key = `${reported.padEnd(24)} ${shape(difference.finding)}`;
        groups.set(key, [...(groups.get(key) ?? []), difference]);
    }
    const sorted = [...groups.entries()].sort((a, b) => b[1].length - a[1].length);
    const out: string[] = [
        `${entries.length} files, ${all.length} differences in ${groups.size} kinds`,
        "(reported by, count, kind; then up to three places)",
        "",
    ];
    if (unchecked.size > 0) {
        out.push(`clang did not check ${unchecked.size} files, compared without it:`);
        for (const [file, reason] of [...unchecked].slice(0, 3)) {
            out.push(`         ${path.relative(root, file)}: ${reason}`);
        }
        out.push("");
    }
    for (const [key, list] of sorted) {
        out.push(`${String(list.length).padStart(5)}  ${key}`);
        for (const difference of list.slice(0, 3)) {
            const lines = TOOLS.filter((tool) => difference.lines[tool] !== undefined)
                .map((tool) => `${tool}:${difference.lines[tool]}`)
                .join(" ");
            out.push(
                `         ${path.relative(root, difference.file)} ${lines} ${difference.finding.name}`,
            );
        }
    }
    return out.join("\n");
}

async function main() {
    const opts = options();
    const entries = selectEntries(opts);
    process.stderr.write(`${entries.length} files\n`);

    const found: Partial<Record<Tool, Map<string, Finding[]>>> = {};
    const clang = new Map<string, Finding[]>();
    const unchecked = new Map<string, string>();
    const queue = [...entries];
    await Promise.all(
        Array.from({ length: opts.jobs }, async () => {
            for (let entry = queue.shift(); entry !== undefined; entry = queue.shift()) {
                const findings = await clangFindings(opts, entry);
                if (typeof findings === "string") {
                    unchecked.set(entrySource(entry), findings);
                } else {
                    clang.set(entrySource(entry), findings);
                }
            }
        }),
    );
    found.clang = clang;
    found.clice = await serverFindings("clice", opts, entries);
    if (opts.clangd !== null) {
        found.clangd = await serverFindings("clangd", opts, entries);
    }
    if (opts.lint) {
        found.lint = await lintFindings(opts);
    }

    if (opts.json !== null) {
        const files = entries.map((entry) => {
            const file = entrySource(entry);
            return {
                file: URI.file(file).toString(),
                clangNotChecked: unchecked.get(file),
                ...Object.fromEntries(
                    TOOLS.filter((tool) => found[tool] !== undefined).map((tool) => [
                        tool,
                        found[tool]?.get(file) ?? [],
                    ]),
                ),
            };
        });
        fs.writeFileSync(opts.json, JSON.stringify(files, null, 2));
    }
    process.stdout.write(`${report(opts.workspace, entries, found, unchecked)}\n`);
}

await main();
