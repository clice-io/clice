/// PCH check: open real files of targets with a precompiled header on two
/// clice servers — one reads the build's database, so it builds each
/// target's PCH and chains every file's preamble PCH onto it; the other
/// reads the same database with every `-include-pch` dropped, so the units
/// parse the text of the header set — and diff what the two answer:
/// diagnostics, semantic tokens, hover and definition at the names of each
/// file, and completion where a name follows `.`, `->` or `::`. Every
/// difference is the precompiled header changing an answer; run it over a
/// CMake project with `target_precompile_headers` (LLVM's lib/ has three)
/// after changing how clice builds or loads PCHs.
///
/// Usage:
///   node tools/bench/pch_check.ts --workspace <dir> --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>          clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --max-positions <N>     names asked per file, sampled evenly beyond (default 300)
///   --completions <N>       member and qualified names completed per file (default 40)
///   --jobs <N>              requests in flight per server (default 8)
///   --json <path>           write every group with its examples as JSON

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    Groups,
    KEYWORDS,
    Lines,
    lex,
    sample,
    type Server,
    snippet,
    start,
    stop,
} from "./lsp_check.ts";

interface Example {
    at: string;
    snippet: string;
    detail?: string;
}

interface Entry {
    command?: string;
    arguments?: string[];
}

/// The database of `cdb` with every `-include-pch` dropped, written to a
/// directory of its own.
function withoutPch(cdb: string): string {
    const entries = JSON.parse(
        fs.readFileSync(path.join(cdb, "compile_commands.json"), "utf8"),
    ) as Entry[];
    for (const entry of entries) {
        if (entry.command !== undefined) {
            entry.command = entry.command
                .replace(/-Xclang -include-pch -Xclang \S+/g, "")
                .replace(/-include-pch \S+/g, "");
        }
        if (entry.arguments !== undefined) {
            const kept: string[] = [];
            for (let i = 0; i < entry.arguments.length; i += 1) {
                const argument = entry.arguments[i];
                if (argument === "-Xclang" && entry.arguments[i + 1] === "-include-pch") {
                    i += 3;
                } else if (argument === "-include-pch") {
                    i += 1;
                } else if (argument !== undefined) {
                    kept.push(argument);
                }
            }
            entry.arguments = kept;
        }
    }
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "pch-check-"));
    fs.writeFileSync(path.join(dir, "compile_commands.json"), JSON.stringify(entries, null, 1));
    return dir;
}

async function pool<T>(items: T[], jobs: number, work: (item: T) => Promise<void>): Promise<void> {
    let next = 0;
    await Promise.all(
        Array.from({ length: Math.max(1, jobs) }, async () => {
            while (next < items.length) {
                const item = items[next];
                next += 1;
                if (item !== undefined) {
                    await work(item);
                }
            }
        }),
    );
}

async function safely<T>(promise: Promise<T>): Promise<T | string> {
    try {
        return await withTimeout(promise, 120_000, "request");
    } catch (error) {
        return `failed: ${String(error)}`;
    }
}

function text(message: string | proto.MarkupContent): string {
    return typeof message === "string" ? message : message.value;
}

/// A location as `path:line:col`, the path relative to the workspace.
function place(workspace: string, uri: string, range: proto.Range): string {
    const file = path.relative(workspace, URI.parse(uri).fsPath);
    return `${file}:${range.start.line + 1}:${range.start.character + 1}`;
}

function locations(
    workspace: string,
    reply: proto.Definition | proto.LocationLink[] | null | string,
): string {
    if (typeof reply === "string" || reply === null) {
        return String(reply);
    }
    const list = Array.isArray(reply) ? reply : [reply];
    return list
        .map((location) =>
            "targetUri" in location
                ? place(workspace, location.targetUri, location.targetSelectionRange)
                : place(workspace, location.uri, location.range),
        )
        .sort()
        .join(", ");
}

function hoverText(reply: proto.Hover | null | string): string {
    if (typeof reply === "string" || reply === null) {
        return String(reply);
    }
    const contents = reply.contents;
    if (Array.isArray(contents)) {
        return contents.map((c) => (typeof c === "string" ? c : c.value)).join("\n");
    }
    return typeof contents === "string" ? contents : contents.value;
}

function diagnosticKey(workspace: string, uri: string, d: proto.Diagnostic): string {
    return `${place(workspace, uri, d.range)} ${d.severity ?? ""} ${text(d.message)}`;
}

function options() {
    const { values, positionals } = parseArgs({
        allowPositionals: true,
        options: {
            clice: { type: "string" },
            workspace: { type: "string" },
            cdb: { type: "string" },
            "max-positions": { type: "string", default: "300" },
            completions: { type: "string", default: "40" },
            jobs: { type: "string", default: "8" },
            json: { type: "string" },
        },
    });
    if (values.workspace === undefined || values.cdb === undefined || positionals.length === 0) {
        console.error(
            "usage: node tools/bench/pch_check.ts --workspace <dir> --cdb <dir> [options] <file>...",
        );
        process.exit(1);
    }
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        workspace: path.resolve(values.workspace),
        cdb: path.resolve(values.cdb),
        positions: Number(values["max-positions"]),
        completions: Number(values.completions),
        jobs: Number(values.jobs),
        json: values.json ?? null,
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    const groups = new Groups<Example>();
    const failures: string[] = [];
    let asked = 0;
    let completed = 0;

    const plainCdb = withoutPch(opts.cdb);
    const chained = await start("clice", opts.clice, opts.workspace, {
        name: "chained",
        cdb: opts.cdb,
    });
    const plain = await start("clice", opts.clice, opts.workspace, {
        name: "plain",
        cdb: plainCdb,
    });
    const servers: Server[] = [chained, plain];

    for (const file of opts.files) {
        const content = fs.readFileSync(file, "utf8");
        const lines = new Lines(content);
        const name = path.relative(opts.workspace, file);
        const at = (offset: number) => {
            const p = lines.position(offset);
            return `${name}:${p.line + 1}:${p.character + 1}`;
        };
        const opened = await Promise.all(
            servers.map(async (server) => {
                try {
                    const [uri] = await withTimeout(
                        server.client.openAndWait(file, 900_000, { text: content }),
                        900_000,
                        `${server.name} open`,
                    );
                    return uri;
                } catch (error) {
                    failures.push(`${server.name} ${name}: ${String(error)}`);
                    return null;
                }
            }),
        );
        const [ours, theirs] = opened;
        if (ours === null || ours === undefined || theirs === null || theirs === undefined) {
            continue;
        }

        const diagnostics = servers.map(
            (server, index) =>
                new Set(
                    (server.client.diagnostics.get(opened[index] ?? "") ?? []).map((d) =>
                        diagnosticKey(opts.workspace, opened[index] ?? "", d),
                    ),
                ),
        );
        for (const [index, own] of diagnostics.entries()) {
            const other = diagnostics[1 - index] ?? new Set<string>();
            for (const key of own) {
                if (!other.has(key)) {
                    groups.add(
                        `diagnostic only ${servers[index]?.name ?? ""}`,
                        `${name}:${key}`,
                        () => ({ at: name, snippet: key }),
                    );
                }
            }
        }

        const [tokensOurs, tokensTheirs] = await Promise.all(
            servers.map((server, index) =>
                safely(server.client.semanticTokensFull(opened[index] ?? "")),
            ),
        );
        const tokenText = (reply: proto.SemanticTokens | null | string | undefined) =>
            typeof reply === "string" || reply === null || reply === undefined
                ? String(reply)
                : reply.data.join(",");
        if (tokenText(tokensOurs) !== tokenText(tokensTheirs)) {
            groups.add("semantic tokens differ", name, () => ({ at: name, snippet: "" }));
        }

        const tokens = lex(content).filter(
            (token) =>
                token.kind === "ident" &&
                !token.directive &&
                !KEYWORDS.has(content.slice(token.begin, token.end)),
        );
        await pool(sample(tokens, opts.positions), opts.jobs, async (token) => {
            const { line, character } = lines.position(token.begin);
            const answers = await Promise.all(
                servers.map(async (server, index) => {
                    const uri = opened[index] ?? "";
                    const [hover, definition] = await Promise.all([
                        safely(server.client.hoverAt(uri, line, character)),
                        safely(server.client.definitionAt(uri, line, character)),
                    ]);
                    return {
                        hover: hoverText(hover),
                        definition: locations(opts.workspace, definition),
                    };
                }),
            );
            asked += 1;
            const [mine, other] = answers;
            if (mine === undefined || other === undefined) {
                return;
            }
            const example = (detail: string) => () => ({
                at: at(token.begin),
                snippet: snippet(lines, token),
                detail,
            });
            if (mine.definition !== other.definition) {
                groups.add(
                    `definition differs: ${content.slice(token.begin, token.end)}`,
                    at(token.begin),
                    example(`chained ${mine.definition} | plain ${other.definition}`),
                );
            }
            if (mine.hover !== other.hover) {
                groups.add(
                    `hover differs: ${content.slice(token.begin, token.end)}`,
                    at(token.begin),
                    example(
                        `chained ${JSON.stringify(mine.hover.slice(0, 300))} | plain ${JSON.stringify(other.hover.slice(0, 300))}`,
                    ),
                );
            }
        });
        const lexed = lex(content);
        const members = lexed.filter((token, i) => {
            const before = lexed[i - 1];
            return (
                token.kind === "ident" &&
                !token.directive &&
                before !== undefined &&
                [".", "->", "::"].includes(content.slice(before.begin, before.end))
            );
        });
        await pool(sample(members, opts.completions), opts.jobs, async (token) => {
            const { line, character } = lines.position(token.begin);
            const [mine, other] = await Promise.all(
                servers.map(async (server, index) => {
                    const reply = await safely(
                        server.client.completionAt(opened[index] ?? "", line, character),
                    );
                    if (typeof reply === "string" || reply === null) {
                        return { labels: String(reply), ranked: String(reply) };
                    }
                    const items = Array.isArray(reply) ? reply : reply.items;
                    // The editor orders the items by sortText, not as sent.
                    const ranked = items
                        .map((item) => `${item.sortText ?? ""} ${item.label}`)
                        .sort()
                        .join("\n");
                    const labels = items
                        .map((item) => item.label)
                        .sort()
                        .join("\n");
                    return { labels, ranked };
                }),
            );
            completed += 1;
            if (mine === undefined || other === undefined) {
                return;
            }
            const differ = (a: string, b: string) => {
                const theirs = new Set(b.split("\n"));
                const only = a.split("\n").filter((item) => !theirs.has(item));
                return only.slice(0, 8).join(", ");
            };
            if (mine.labels !== other.labels) {
                groups.add("completion items differ", at(token.begin), () => ({
                    at: at(token.begin),
                    snippet: snippet(lines, token),
                    detail: `chained only ${differ(mine.labels, other.labels)} | plain only ${differ(other.labels, mine.labels)}`,
                }));
            } else if (mine.ranked !== other.ranked) {
                groups.add("completion ranks differ", at(token.begin), () => ({
                    at: at(token.begin),
                    snippet: snippet(lines, token),
                    detail: `chained ${differ(mine.ranked, other.ranked)} | plain ${differ(other.ranked, mine.ranked)}`,
                }));
            }
        });
        for (const [index, server] of servers.entries()) {
            server.client.close(opened[index] ?? "");
        }
        console.error(`checked ${name}`);
    }

    for (const server of servers) {
        await stop(server);
    }
    fs.rmSync(plainCdb, { recursive: true, force: true });

    console.log(
        `# PCH check: ${opts.files.length} files, ${asked} names asked, ${completed} completed\n`,
    );
    for (const [key, group] of groups.sorted()) {
        console.log(`- ${group.count} × ${key}`);
        for (const e of group.examples) {
            console.log(
                `    ${e.at}  ${e.snippet}${e.detail === undefined ? "" : `  [${e.detail}]`}`,
            );
        }
    }
    if (failures.length > 0) {
        console.log(`\n## Failures\n`);
        for (const failure of failures) {
            console.log(`- ${failure}`);
        }
    }
    if (opts.json !== null) {
        fs.writeFileSync(
            opts.json,
            JSON.stringify(
                {
                    groups: groups.sorted().map(([key, group]) => ({
                        key,
                        count: group.count,
                        examples: group.examples,
                    })),
                    failures,
                },
                null,
                1,
            ),
        );
    }
}

await main();
