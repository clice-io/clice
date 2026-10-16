/// Semantic tokens check: ask clice — and, given a binary, clangd — for the
/// semantic tokens of real files, and clice's `inspect semantic-tokens
/// --check` for where the tokens its index projects differ from the AST's;
/// group every difference by its shape: which kinds, which modifiers, the
/// punctuation around the token.
///
/// clice runs with its kinds and modifiers renamed to clangd's names for
/// the same things, through its own `[semantic_tokens]` mapping, so the two
/// compare by name; what only one legend names is not compared.
///
/// Usage:
///   node tools/bench/semantic_tokens_check.ts --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>     clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>    clangd executable; no clangd diff without it
///   --no-index         skip the index projection check
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
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import { Groups, Lines, type Server, snippet, start, stop } from "./lsp_check.ts";

/// A decoded token, at UTF-16 offsets of the document.
interface Token {
    begin: number;
    end: number;
    type: string;
    modifiers: string[];
}

interface Example {
    at: string;
    snippet: string;
    detail?: string;
}

/// clice's kinds and modifiers by clangd's name for what they mark.
const CLANGD_NAMES = {
    kinds: {
        struct: "class",
        union: "class",
        module: "namespace",
        typeAlias: "type",
        templateParameter: "parameter",
        field: "property",
    },
    modifiers: {
        local: "functionScope",
        member: "classScope",
        internal: "fileScope",
        global: "globalScope",
        dependent: "dependentName",
        modification: "usedAsMutableReference",
        constructor: "constructorOrDestructor",
        destructor: "constructorOrDestructor",
    },
};

const ROLES = ["definition", "declaration"];
const SCOPES = ["functionScope", "classScope", "fileScope", "globalScope"];

function legendOf(server: Server): proto.SemanticTokensLegend {
    const provider = server.client.initResult?.capabilities.semanticTokensProvider as
        | proto.SemanticTokensOptions
        | undefined;
    if (provider === undefined) {
        throw new Error(`${server.name} offers no semantic tokens`);
    }
    return provider.legend;
}

function decode(data: number[], legend: proto.SemanticTokensLegend, lines: Lines): Token[] {
    const tokens: Token[] = [];
    let line = 0;
    let character = 0;
    for (let i = 0; i + 4 < data.length; i += 5) {
        const deltaLine = data[i] ?? 0;
        line += deltaLine;
        character = deltaLine === 0 ? character + (data[i + 1] ?? 0) : (data[i + 1] ?? 0);
        const begin = (lines.starts[line] ?? lines.text.length) + character;
        const bits = data[i + 4] ?? 0;
        tokens.push({
            begin,
            end: begin + (data[i + 2] ?? 0),
            type: legend.tokenTypes[data[i + 3] ?? 0] ?? "?",
            modifiers: legend.tokenModifiers.filter((_, bit) => (bits & (1 << bit)) !== 0),
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

/// The modifiers `shared` names, a token's role and its scope each folded
/// into one value: clangd marks a definition as a declaration too.
function normalized(token: Token, shared: readonly string[]): Map<string, string> {
    const values = new Map<string, string>();
    values.set("role", ROLES.find((r) => token.modifiers.includes(r)) ?? "reference");
    values.set("scope", SCOPES.find((s) => token.modifiers.includes(s)) ?? "none");
    for (const modifier of shared) {
        if (!ROLES.includes(modifier) && !SCOPES.includes(modifier)) {
            values.set(modifier, token.modifiers.includes(modifier) ? "yes" : "no");
        }
    }
    return values;
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
        json: values.json ?? null,
        dump: values.dump === undefined ? null : path.resolve(values.dump),
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    const differences = new Groups<Example>();
    const indexGroups = new Groups<Example>();
    const counts = new Map<string, number>();
    const failures: string[] = [];
    let tokenCount = 0;

    const servers = [
        await start("clice", opts.clice, opts.cdb, {
            config: { semantic_tokens: CLANGD_NAMES },
        }),
    ];
    if (opts.clangd !== null) {
        servers.push(await start("clangd", opts.clangd, opts.cdb));
    }
    const legends = servers.map(legendOf);
    const [ourLegend, theirLegend] = legends;
    const sharedTypes = new Set(
        ourLegend?.tokenTypes.filter((t) => theirLegend?.tokenTypes.includes(t)) ?? [],
    );
    const sharedModifiers =
        ourLegend?.tokenModifiers.filter((m) => theirLegend?.tokenModifiers.includes(m)) ?? [];

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
            servers.map(async (server, index) => {
                try {
                    const [uri] = await withTimeout(
                        server.client.openAndWait(file, 600_000, { text }),
                        600_000,
                        `${server.name} open`,
                    );
                    const errors = (server.client.diagnostics.get(uri) ?? []).filter(
                        (d) => d.severity === proto.DiagnosticSeverity.Error,
                    );
                    const first = errors[0];
                    if (first !== undefined) {
                        const message =
                            typeof first.message === "string" ? first.message : first.message.value;
                        failures.push(
                            `${server.name} ${name}: ${errors.length} errors, first at ${first.range.start.line + 1}: ${message}`,
                        );
                    }
                    const reply = await withTimeout(
                        server.client.semanticTokensFull(uri),
                        600_000,
                        `${server.name} semanticTokens`,
                    );
                    server.client.close(uri);
                    const legend = legends[index];
                    return legend === undefined ? null : decode(reply?.data ?? [], legend, lines);
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
            const spelled = (t: Token) => text.slice(t.begin, t.end);
            const example = (t: Token, detail?: string) => () => ({
                at: at(t.begin),
                snippet: snippet(lines, t),
                ...(detail === undefined ? {} : { detail }),
            });
            // clangd paints inactive code as comment lines.
            const compared = (t: Token) =>
                sharedTypes.has(t.type) &&
                t.type !== "comment" &&
                !t.modifiers.includes("inactive");
            const mine = new Map(ours.filter(compared).map((t) => [t.begin, t]));
            for (const other of theirs.filter(compared)) {
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
                            ? `clangd only: ${other.type} ${where}`
                            : `range: clice ${covering.type} covers clangd ${other.type}`;
                    differences.add(
                        key,
                        `${name}:${other.begin}`,
                        example(other, covering && spelled(covering)),
                    );
                    continue;
                }
                mine.delete(other.begin);
                if (token.type !== other.type) {
                    differences.add(
                        `kind: clice ${token.type} vs clangd ${other.type} ${where}`,
                        `${name}:${token.begin}`,
                        example(token),
                    );
                    continue;
                }
                if (token.end !== other.end) {
                    differences.add(
                        `range: ${token.type} clice \`${spelled(token)}\` vs clangd \`${spelled(other)}\``,
                        `${name}:${token.begin}`,
                        example(token),
                    );
                }
                const a = normalized(token, sharedModifiers);
                const b = normalized(other, sharedModifiers);
                for (const [modifier, value] of a) {
                    if (value !== b.get(modifier)) {
                        differences.add(
                            `modifier ${modifier}: clice ${value} vs clangd ${b.get(modifier) ?? "?"} on ${token.type} ${where}`,
                            `${name}:${token.begin}:${modifier}`,
                            example(token),
                        );
                    }
                }
            }
            for (const token of mine.values()) {
                differences.add(
                    `clice only: ${token.type} ${context(text, token.begin, token.end)}`,
                    `${name}:${token.begin}`,
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
                    indexGroups.add(
                        `${difference.field}: ast ${difference.ast} | index ${difference.index}`,
                        `${name}:${difference.line}:${difference.column}`,
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

    const print = (title: string, groups: Groups<Example>) => {
        console.log(`\n## ${title}\n`);
        for (const [key, group] of groups.sorted()) {
            console.log(`- ${group.count} × ${key}`);
            for (const e of group.examples) {
                console.log(
                    `    ${e.at}  ${e.snippet}${e.detail === undefined ? "" : `  [${e.detail}]`}`,
                );
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
        const dump = (groups: Groups<Example>) =>
            groups.sorted().map(([key, group]) => ({
                key,
                count: group.count,
                examples: group.examples,
            }));
        fs.writeFileSync(
            opts.json,
            JSON.stringify(
                { clangd: dump(differences), index: dump(indexGroups), failures },
                null,
                1,
            ),
        );
    }
}

await main();
