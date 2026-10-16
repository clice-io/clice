/// Semantic tokens check: ask clice — and, given a binary, clangd — for the
/// semantic tokens of real files, and clice's `inspect semantic-tokens
/// --check` for where the tokens its index projects differ from the AST's;
/// group every difference by its shape: which kinds, which modifiers, the
/// punctuation around the token.
///
/// Usage:
///   node tools/bench/semantic_tokens_check.ts --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>     clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>    clangd executable; no clangd diff without it
///   --no-index         skip the index projection check
///   --examples <N>     examples per group (default 4)
///   --json <path>      write every group with its examples as JSON
///   --dump <file>      print each server's tokens of one file side by side
///                      instead of checking
///
/// The CDB directory is the workspace of both servers and must hold a
/// compile_commands.json naming every file.

import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { promisify } from "node:util";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";

/// A decoded token, at UTF-16 offsets of the document.
interface Token {
    begin: number;
    end: number;
    type: string;
    modifiers: string[];
}

/// clice's kinds by the clangd type that names the same thing; a clice
/// kind missing here is one clangd has no type for (literals, keywords,
/// directives, punctuation), never compared.
const COUNTERPARTS: Record<string, string[]> = {
    namespace: ["namespace"],
    module: ["namespace"],
    class: ["class"],
    struct: ["class"],
    union: ["class"],
    enum: ["enum"],
    type: ["type", "typeParameter", "class", "enum"],
    field: ["property"],
    enumMember: ["enumMember"],
    function: ["function"],
    method: ["method"],
    variable: ["variable"],
    parameter: ["parameter", "typeParameter"],
    label: ["label"],
    concept: ["concept"],
    macro: ["macro"],
    operator: ["operator"],
    keyword: ["modifier"],
    angle: ["bracket"],
};

const OVERRIDES = ["override", "final", "sealed"];

/// Modifiers both legends name; the others are one server's own.
const SHARED_MODIFIERS = [
    "declaration",
    "definition",
    "deprecated",
    "readonly",
    "static",
    "abstract",
    "virtual",
    "dependentName",
    "defaultLibrary",
    "usedAsMutableReference",
    "usedAsMutablePointer",
    "constructorOrDestructor",
    "userDefined",
    "functionScope",
    "classScope",
    "fileScope",
    "globalScope",
];

class Lines {
    readonly starts: number[] = [0];
    readonly text: string;

    constructor(text: string) {
        this.text = text;
        for (let i = 0; i < text.length; i += 1) {
            if (text[i] === "\n") {
                this.starts.push(i + 1);
            }
        }
    }

    offset(line: number, character: number): number {
        return (this.starts[line] ?? this.text.length) + character;
    }

    position(offset: number): { line: number; character: number } {
        let low = 0;
        let high = this.starts.length - 1;
        while (low < high) {
            const mid = (low + high + 1) >> 1;
            if ((this.starts[mid] ?? 0) <= offset) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        return { line: low, character: offset - (this.starts[low] ?? 0) };
    }

    /// The token's line with the token marked: `a ⟦b⟧ c`.
    snippet(begin: number, end: number): string {
        const { line } = this.position(begin);
        const from = this.starts[line] ?? 0;
        let to = this.text.indexOf("\n", from);
        to = to < 0 ? this.text.length : to;
        const stop = Math.min(end, to);
        const marked = `${this.text.slice(from, begin)}⟦${this.text.slice(begin, stop)}⟧${this.text.slice(stop, to)}`;
        return marked.trim().replace(/\s+/g, " ").slice(0, 160);
    }
}

function decode(data: number[], legend: proto.SemanticTokensLegend, lines: Lines): Token[] {
    const tokens: Token[] = [];
    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const deltaLine = data[i] ?? 0;
        line += deltaLine;
        character = deltaLine === 0 ? character + (data[i + 1] ?? 0) : (data[i + 1] ?? 0);
        const begin = lines.offset(line, character);
        const bits = data[i + 4] ?? 0;
        const modifiers = legend.tokenModifiers.filter((_, bit) => (bits & (1 << bit)) !== 0);
        tokens.push({
            begin,
            end: begin + (data[i + 2] ?? 0),
            type: legend.tokenTypes[data[i + 3] ?? 0] ?? "?",
            modifiers,
        });
    }
    return tokens;
}

/// The written context of a token: the punctuation right before and after
/// it that tells name shapes apart (`a.b`, `A::b`, `~A`, `f<`, `f(`).
function context(text: string, begin: number, end: number): string {
    const before = /(::|->|\.|~|&|\*|<|#)\s*$/.exec(text.slice(Math.max(0, begin - 8), begin));
    const after = /^\s*(::|<|\(|\{|=|;)/.exec(text.slice(end, end + 8));
    return `${before?.[1] ?? ""}_${after?.[1] ?? ""}`;
}

const ROLES = ["definition", "declaration"];
const SCOPES = ["functionScope", "classScope", "fileScope", "globalScope"];

/// The modifiers to compare, a definition's role and a token's scope each
/// folded into one value: clangd marks a definition as both a declaration
/// and a definition.
function normalized(token: Token): Map<string, string> {
    const values = new Map<string, string>();
    const role = ROLES.find((r) => token.modifiers.includes(r));
    values.set("role", role ?? "reference");
    values.set("scope", SCOPES.find((s) => token.modifiers.includes(s)) ?? "none");
    for (const modifier of SHARED_MODIFIERS) {
        if (!ROLES.includes(modifier) && !SCOPES.includes(modifier)) {
            values.set(modifier, token.modifiers.includes(modifier) ? "yes" : "no");
        }
    }
    return values;
}

interface Example {
    at: string;
    snippet: string;
    detail?: string;
}

class Groups {
    readonly groups = new Map<string, { count: number; files: Set<string>; examples: Example[] }>();

    private readonly limit: number;

    constructor(limit: number) {
        this.limit = limit;
    }

    add(key: string, file: string, example: () => Example): void {
        let group = this.groups.get(key);
        if (group === undefined) {
            group = { count: 0, files: new Set(), examples: [] };
            this.groups.set(key, group);
        }
        group.count += 1;
        // Examples from as many files as possible.
        if (group.examples.length < this.limit && !group.files.has(file)) {
            group.examples.push(example());
        }
        group.files.add(file);
    }

    sorted() {
        return [...this.groups].sort((a, b) => b[1].count - a[1].count);
    }
}

interface Server {
    name: "clice" | "clangd";
    client: CliceClient;
    legend: proto.SemanticTokensLegend;
}

const CAPABILITIES: proto.ClientCapabilities = {
    textDocument: {
        semanticTokens: {
            requests: { full: true },
            tokenTypes: [],
            tokenModifiers: [],
            formats: ["relative"],
            multilineTokenSupport: false,
            overlappingTokenSupport: false,
        },
        publishDiagnostics: {},
    },
};

async function start(name: "clice" | "clangd", binary: string, workspace: string): Promise<Server> {
    const client = CliceClient.start(binary, {
        args:
            name === "clice"
                ? ["serve"]
                : [`--compile-commands-dir=${workspace}`, "--background-index=false", "-j=2"],
        cwd: workspace,
    });
    await client.initialize(new Workspace(workspace), {
        testDefaults: false,
        capabilities: CAPABILITIES,
        initializationOptions:
            name === "clice"
                ? {
                      rules: [{ compile_commands: [workspace] }],
                      project: { enable_indexing: false, stateful_worker_count: 1 },
                  }
                : {},
    });
    const provider = client.initResult?.capabilities.semanticTokensProvider as
        | proto.SemanticTokensOptions
        | undefined;
    if (provider === undefined) {
        throw new Error(`${name} offers no semantic tokens`);
    }
    return { name, client, legend: provider.legend };
}

async function stop(server: Server): Promise<void> {
    if (server.name === "clice") {
        await server.client.shutdown();
        return;
    }
    try {
        await withTimeout(server.client.sendRequest(proto.ShutdownRequest.type), 60_000, "shutdown");
        await server.client.sendNotification(proto.ExitNotification.type);
    } catch {
        // Killed below.
    }
    server.client.disposed = true;
    setTimeout(() => server.client.child.kill("SIGKILL"), 5_000).unref();
    await server.client.exited;
}

interface IndexDifference {
    line: number;
    column: number;
    text: string;
    field: string;
    ast: string;
    index: string;
}

/// Where the index projection differs from the AST, per `inspect --check`.
async function indexDifferences(clice: string, file: string): Promise<IndexDifference[] | string> {
    try {
        const { stdout } = await promisify(execFile)(
            clice,
            ["inspect", "semantic-tokens", file, "--check"],
            { cwd: path.dirname(file), maxBuffer: 1 << 30, timeout: 600_000 },
        );
        const output = JSON.parse(stdout) as {
            files: Record<string, { result?: { differences: IndexDifference[] }; error?: string }>;
        };
        const entry = Object.values(output.files)[0];
        if (entry?.result === undefined) {
            return entry?.error ?? "no result";
        }
        return entry.result.differences;
    } catch (error) {
        return String(error);
    }
}

function options() {
    const { values, positionals } = parseArgs({
        allowPositionals: true,
        options: {
            clice: { type: "string" },
            clangd: { type: "string" },
            cdb: { type: "string" },
            "no-index": { type: "boolean", default: false },
            examples: { type: "string", default: "4" },
            json: { type: "string" },
            dump: { type: "string" },
        },
    });
    if (values.cdb === undefined || (positionals.length === 0 && values.dump === undefined)) {
        console.error(
            "usage: node tools/bench/semantic_tokens_check.ts --cdb <dir> [options] <file>...",
        );
        process.exit(1);
    }
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        clangd: values.clangd ?? null,
        cdb: path.resolve(values.cdb),
        index: !values["no-index"],
        examples: Number(values.examples),
        json: values.json ?? null,
        dump: values.dump === undefined ? null : path.resolve(values.dump),
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    const differences = new Groups(opts.examples);
    const indexGroups = new Groups(opts.examples);
    const counts = new Map<string, number>();
    const failures: string[] = [];
    let tokenCount = 0;

    const servers = [await start("clice", opts.clice, opts.cdb)];
    if (opts.clangd !== null) {
        servers.push(await start("clangd", opts.clangd, opts.cdb));
    }

    const files = opts.dump === null ? opts.files : [opts.dump];
    for (const file of files) {
        const text = fs.readFileSync(file, "utf8").replace(/^﻿/, "");
        const lines = new Lines(text);
        const name = path.relative(opts.cdb, file);
        const at = (offset: number) => {
            const p = lines.position(offset);
            return `${name}:${p.line + 1}:${p.character + 1}`;
        };

        const results = await Promise.all(
            servers.map(async (server) => {
                try {
                    const [uri] = await withTimeout(
                        server.client.openAndWait(file, 600_000, { text }),
                        600_000,
                        `${server.name} open`,
                    );
                    const errors = (server.client.diagnostics.get(uri) ?? []).filter(
                        (d) => d.severity === proto.DiagnosticSeverity.Error,
                    );
                    if (errors.length > 0) {
                        const first = errors[0];
                        const message =
                            typeof first?.message === "string" ? first.message : first?.message.value;
                        failures.push(
                            `${server.name} ${name}: ${errors.length} errors, first at ${(first?.range.start.line ?? 0) + 1}: ${message}`,
                        );
                    }
                    const reply = await withTimeout(
                        server.client.semanticTokensFull(uri),
                        600_000,
                        `${server.name} semanticTokens`,
                    );
                    server.client.close(uri);
                    return decode(reply?.data ?? [], server.legend, lines);
                } catch (error) {
                    failures.push(`${server.name} ${name}: ${String(error)}`);
                    return null;
                }
            }),
        );
        const [ours, theirs] = results;
        if (ours === null || ours === undefined) {
            continue;
        }
        tokenCount += ours.length;
        for (const token of ours) {
            counts.set(token.type, (counts.get(token.type) ?? 0) + 1);
        }

        if (opts.dump !== null) {
            const show = (token: Token | undefined) =>
                token === undefined ? "" : `${token.type} ${token.modifiers.join(" ")}`;
            const byBegin = new Map((theirs ?? []).map((t) => [t.begin, t]));
            for (const token of ours) {
                const other = byBegin.get(token.begin);
                byBegin.delete(token.begin);
                console.log(
                    `${at(token.begin).padEnd(28)} ${text.slice(token.begin, token.end).padEnd(20)} ${show(token).padEnd(60)} | ${show(other)}`,
                );
            }
            for (const token of byBegin.values()) {
                console.log(
                    `${at(token.begin).padEnd(28)} ${text.slice(token.begin, token.end).padEnd(20)} ${"".padEnd(60)} | ${show(token)}`,
                );
            }
            continue;
        }

        if (theirs !== null && theirs !== undefined) {
            const mine = new Map(ours.map((t) => [t.begin, t]));
            const spelled = (t: Token) => text.slice(t.begin, t.end);
            const example = (t: Token, detail?: string) => () => ({
                at: at(t.begin),
                snippet: lines.snippet(t.begin, t.end),
                ...(detail === undefined ? {} : { detail }),
            });
            // clangd paints inactive code as comment lines and keywords
            // not at all but `override` and `final`.
            const compared = (t: Token) =>
                !t.modifiers.includes("inactive") &&
                (t.type !== "keyword" || OVERRIDES.includes(spelled(t)));
            for (const t of ours) {
                if (!compared(t)) {
                    mine.delete(t.begin);
                }
            }
            for (const other of theirs) {
                if (other.type === "comment") {
                    continue;
                }
                const token = mine.get(other.begin);
                const where = context(text, other.begin, other.end);
                if (token === undefined) {
                    // A clice token covering it from an earlier begin is a
                    // range difference.
                    const covering = ours.find(
                        (t) => compared(t) && t.begin < other.begin && t.end > other.begin,
                    );
                    const key =
                        covering === undefined
                            ? `clangd only: ${other.type} [${other.modifiers.filter((m) => SHARED_MODIFIERS.includes(m) && !SCOPES.includes(m)).join(" ")}] ${where}`
                            : `range: clice ${covering.type} covers clangd ${other.type}`;
                    differences.add(key, name, example(other, covering && spelled(covering)));
                    continue;
                }
                mine.delete(other.begin);
                const counterparts = COUNTERPARTS[token.type];
                if (counterparts === undefined) {
                    differences.add(
                        `kind: clice ${token.type} (no clangd counterpart) vs clangd ${other.type} ${where}`,
                        name,
                        example(token),
                    );
                    continue;
                }
                if (!counterparts.includes(other.type)) {
                    differences.add(
                        `kind: clice ${token.type} vs clangd ${other.type} ${where}`,
                        name,
                        example(token),
                    );
                    continue;
                }
                if (token.end !== other.end) {
                    differences.add(
                        `range: ${token.type} clice \`${spelled(token)}\` vs clangd \`${spelled(other)}\``,
                        name,
                        example(token),
                    );
                }
                const a = normalized(token);
                const b = normalized(other);
                for (const [modifier, value] of a) {
                    const theirs = b.get(modifier);
                    if (value !== theirs) {
                        differences.add(
                            `modifier ${modifier}: clice ${value} vs clangd ${theirs ?? "?"} on ${token.type}/${other.type} ${where}`,
                            name,
                            example(token),
                        );
                    }
                }
            }
            for (const token of mine.values()) {
                if (COUNTERPARTS[token.type] === undefined) {
                    continue;
                }
                differences.add(
                    `clice only: ${token.type} ${context(text, token.begin, token.end)}`,
                    name,
                    example(token),
                );
            }
        }

        if (opts.index) {
            const found = await indexDifferences(opts.clice, file);
            if (typeof found === "string") {
                failures.push(`clice inspect ${name}: ${found.slice(0, 300)}`);
            } else {
                for (const difference of found) {
                    const strip = (s: string) =>
                        s
                            .split(" ")
                            .filter((w) => !/Scope$|^Readonly$|^Static$/.test(w))
                            .join(" ");
                    indexGroups.add(
                        `${difference.field}: ast ${strip(difference.ast)} | index ${strip(difference.index)}`,
                        name,
                        () => ({
                            at: `${name}:${difference.line}:${difference.column}`,
                            snippet: difference.text.slice(0, 80),
                        }),
                    );
                }
            }
        }
    }

    for (const server of servers) {
        await stop(server);
    }
    if (opts.dump !== null) {
        return;
    }

    const print = (title: string, groups: Groups) => {
        console.log(`\n## ${title}\n`);
        for (const [key, group] of groups.sorted()) {
            console.log(`- ${group.count} × ${key}  (${group.files.size} files)`);
            for (const e of group.examples) {
                console.log(`    ${e.at}  ${e.snippet}${e.detail === undefined ? "" : `  [${e.detail}]`}`);
            }
        }
    };
    console.log(`# Semantic tokens check: ${files.length} files, ${tokenCount} clice tokens\n`);
    console.log(
        [...counts]
            .sort((a, b) => b[1] - a[1])
            .map(([type, count]) => `${type} ${count}`)
            .join(", "),
    );
    if (opts.clangd !== null) {
        print("clice vs clangd", differences);
    }
    if (opts.index) {
        print("index projection vs AST", indexGroups);
    }
    if (failures.length > 0) {
        console.log(`\n## Failures\n`);
        for (const failure of failures) {
            console.log(`- ${failure}`);
        }
    }
    if (opts.json !== null) {
        const dump = (groups: Groups) =>
            groups.sorted().map(([key, group]) => ({
                key,
                count: group.count,
                files: group.files.size,
                examples: group.examples,
            }));
        fs.writeFileSync(
            opts.json,
            JSON.stringify({ clangd: dump(differences), index: dump(indexGroups), failures }, null, 1),
        );
    }
}

await main();
