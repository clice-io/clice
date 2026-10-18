/// Type display check: ask clice — and, given a binary, clangd — how they
/// spell the types of real files wherever a reader sees one: the type hints
/// of a whole document, the hover card of each hinted declaration, the
/// completions after a member access and the signature help of a call. Every
/// spelling is tagged with the kinds of spelling it holds (a library's
/// internal name, a member typedef of a specialization, a builtin standing
/// for an alias, defaulted template arguments, …), counted per feature, and
/// clice's set beside clangd's where they differ.
///
/// Usage:
///   node tools/bench/type_display_check.ts --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>         clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>        clangd executable; no comparison without it
///   --unlimited            no length limit on type hints, for either server
///   --completions <n>      member accesses completed per file (default 8)
///   --signatures <n>       calls asked for signature help per file (default 8)
///   --json <path>          write every spelling as JSON
///
/// The CDB directory is the workspace of both servers and must hold a
/// compile_commands.json naming every file. type_display_tour.cpp beside
/// this file is everyday standard library code that real projects use too
/// little of (ranges, chrono, smart pointers, callables): worth a run of its
/// own, once against libstdc++ and once against libc++.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    Groups,
    KEYWORDS,
    Lines,
    type Server,
    lex,
    sample,
    snippet,
    start,
    stop,
} from "./lsp_check.ts";

type Feature = "hint" | "hover" | "completion" | "signature";

/// One place a type is shown: what each server shows there, null when it
/// shows nothing.
interface Shown {
    feature: Feature;
    at: string;
    snippet: string;
    /// What is shown: the declaration hovered, the member completed.
    subject: string;
    clice: string | null;
    clangd: string | null;
    /// A hover's desugared form.
    cliceAka?: string | null;
    clangdAka?: string | null;
}

interface Example {
    at: string;
    snippet: string;
    detail?: string;
}

const RESERVED = /(?<![\w$])(?:__\w+|_[A-Z]\w*)/;
const BUILTIN_INTEGER =
    /^(?:const )?(?:unsigned |signed )?(?:char|short|int|long|long long|long int|unsigned)(?: [*&]+)?$/;

/// The kinds of spelling `text` holds.
export function kinds(text: string): string[] {
    const found: string[] = [];
    const tag = (kind: string, matched: boolean) => {
        if (matched) {
            found.push(kind);
        }
    };
    tag("library internal name", RESERVED.test(text));
    tag("builtin integer", BUILTIN_INTEGER.test(text.trim()));
    tag("member typedef of a specialization", /<.*>::\w+/.test(text));
    tag("basic_string<…>", /\bbasic_string(?:_view)?</.test(text));
    tag(
        "defaulted template argument",
        /\b(?:allocator|char_traits|less|equal_to|hash|default_delete)</.test(text),
    );
    tag("lambda", /\(lambda|\blambda\b/.test(text));
    tag("dependent placeholder", /type-parameter-|<dependent type>|\bdecltype\(/.test(text));
    tag("elided", text.includes("…"));
    tag("chrono duration", /\bduration<|\btime_point</.test(text));
    tag(
        "view or adaptor type",
        /_view\b|_view<|\b__?adaptor|_Partial|_Pipe\b|_RangeAdaptor/.test(text),
    );
    tag("function type", /\(\*?\)?\(|\(\*\)|\)\(/.test(text));
    tag("std:: qualified", /\bstd::/.test(text));
    tag("longer than 40", text.length > 40);
    return found.length === 0 ? ["plain"] : found;
}

/// The text a type hint shows, its `: ` or `-> ` included.
function hintLabel(hint: proto.InlayHint): string {
    return typeof hint.label === "string"
        ? hint.label
        : hint.label.map((part) => part.value).join("");
}

/// The type a hover card gives its declaration, with its `aka`; null when
/// it gives none.
function hoverType(
    reply: proto.Hover | null | undefined,
): { type: string; aka: string | null } | null {
    if (reply === null || reply === undefined) {
        return null;
    }
    const contents = reply.contents;
    const text =
        typeof contents === "string"
            ? contents
            : Array.isArray(contents)
              ? contents.map((c) => (typeof c === "string" ? c : c.value)).join("\n")
              : contents.value;
    const match = /(?:Type|→): `([^`]*)`(?: \(aka `([^`]*)`\))?/.exec(text);
    if (match === null) {
        return null;
    }
    // clangd writes the aka inside the code span.
    const inside = /^(.*) \(aka (.*)\)$/.exec(match[1] ?? "");
    if (inside !== null) {
        return { type: inside[1] ?? "", aka: inside[2] ?? null };
    }
    return { type: match[1] ?? "", aka: match[2] ?? null };
}

/// The name a completion item completes.
function itemName(item: proto.CompletionItem): string {
    const text = (item.filterText ?? item.label).trim().replace(/^•/, "");
    return /^~?\w+/.exec(text)?.[0] ?? text;
}

/// The type a completion item shows: clice's labelDetails description,
/// clangd's detail.
function itemType(server: Server, item: proto.CompletionItem): string | null {
    const type = server.kind === "clice" ? item.labelDetails?.description : item.detail;
    return type === undefined || type === "" ? null : type;
}

const CAPABILITIES: proto.ClientCapabilities = {
    textDocument: {
        hover: { contentFormat: [proto.MarkupKind.Markdown] },
        inlayHint: {},
        completion: {
            completionItem: { labelDetailsSupport: true, snippetSupport: false },
        },
        signatureHelp: {
            signatureInformation: { parameterInformation: { labelOffsetSupport: true } },
        },
    },
};

interface Options {
    clice: string;
    clangd: string | null;
    cdb: string;
    unlimited: boolean;
    completions: number;
    signatures: number;
    json: string | null;
    files: string[];
}

function options(): Options {
    const { values, positionals } = parseArgs({
        allowPositionals: true,
        options: {
            clice: { type: "string" },
            clangd: { type: "string" },
            cdb: { type: "string" },
            unlimited: { type: "boolean", default: false },
            completions: { type: "string", default: "8" },
            signatures: { type: "string", default: "8" },
            json: { type: "string" },
        },
    });
    if (values.cdb === undefined || positionals.length === 0) {
        console.error(
            "usage: node tools/bench/type_display_check.ts --cdb <dir> [options] <file>...",
        );
        process.exit(1);
    }
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        clangd: values.clangd ?? null,
        cdb: path.resolve(values.cdb),
        unlimited: values.unlimited,
        completions: Number(values.completions),
        signatures: Number(values.signatures),
        json: values.json ?? null,
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    if (opts.unlimited) {
        // clangd reads its limit from the user's configuration.
        const home = fs.mkdtempSync(path.join(os.tmpdir(), "type-display-"));
        fs.mkdirSync(path.join(home, "clangd"));
        fs.writeFileSync(
            path.join(home, "clangd/config.yaml"),
            "InlayHints:\n  TypeNameLimit: 0\n",
        );
        process.env["XDG_CONFIG_HOME"] = home;
    }
    const serve = (kind: "clice" | "clangd", binary: string) =>
        start(kind, binary, opts.cdb, {
            capabilities: CAPABILITIES,
            config: opts.unlimited ? { inlay_hints: { max_length: { types: 0 } } } : undefined,
        });
    const servers = [await serve("clice", opts.clice)];
    if (opts.clangd !== null) {
        servers.push(await serve("clangd", opts.clangd));
    }
    const shown: Shown[] = [];
    const failures: string[] = [];

    const ask = async <T>(server: Server, what: string, request: () => Promise<T>) => {
        try {
            return await withTimeout(request(), 120_000, `${server.name} ${what}`);
        } catch (error) {
            failures.push(`${server.name} ${what}: ${String(error)}`);
            return null;
        }
    };

    for (const file of opts.files) {
        const text = fs.readFileSync(file, "utf8").replace(/^﻿/, "");
        const lines = new Lines(text);
        const tokens = lex(text);
        const name = path.relative(opts.cdb, file);
        const at = (offset: number) => {
            const p = lines.position(offset);
            return `${name}:${p.line + 1}:${p.character + 1}`;
        };
        const uris = await Promise.all(
            servers.map(async (server) => {
                const opened = await ask(server, `open ${name}`, () =>
                    server.client.openAndWait(file, 120_000, { text }),
                );
                return opened?.[0] ?? null;
            }),
        );
        if (uris.some((uri) => uri === null)) {
            continue;
        }
        const uri = (index: number) => uris[index] ?? "";
        const whole = { start: { line: 0, character: 0 }, end: lines.position(text.length) };

        // Type hints, by the offset they show at and their `: ` or `-> `.
        const hints = new Map<string, { offset: number; texts: (string | null)[] }>();
        for (const [index, server] of servers.entries()) {
            const reply = await ask(server, `inlayHint ${name}`, () =>
                server.client.inlayHints(uri(index), whole),
            );
            for (const hint of reply ?? []) {
                const label = hintLabel(hint).trim();
                const prefix = /^(:|->)\s*/.exec(label);
                const offset = lines.offset(hint.position);
                if (prefix === null || typeof offset !== "number") {
                    continue;
                }
                const key = `${offset}${prefix[1] ?? ""}`;
                const entry = hints.get(key) ?? { offset, texts: servers.map(() => null) };
                entry.texts[index] = label.slice(prefix[0].length);
                hints.set(key, entry);
            }
        }
        for (const [key, { offset, texts }] of [...hints].sort(
            (a, b) => a[1].offset - b[1].offset,
        )) {
            const subject = /\w+$/.exec(text.slice(Math.max(0, offset - 64), offset))?.[0] ?? "";
            shown.push({
                feature: "hint",
                at: at(offset),
                snippet: snippet(lines, { begin: offset, end: offset }),
                subject: key.endsWith("->") ? "lambda return" : subject,
                clice: texts[0] ?? null,
                clangd: texts[1] ?? null,
            });
            if (key.endsWith("->") || subject === "") {
                continue;
            }
            // The hover card of the declaration the hint follows.
            const position = lines.position(offset - 1);
            const cards = await Promise.all(
                servers.map((server, index) =>
                    ask(server, `hover ${at(offset - 1)}`, () =>
                        server.client.hoverAt(uri(index), position.line, position.character),
                    ),
                ),
            );
            const [ours, theirs] = cards.map(hoverType);
            shown.push({
                feature: "hover",
                at: at(offset - 1),
                snippet: snippet(lines, { begin: offset - subject.length, end: offset }),
                subject,
                clice: ours?.type ?? null,
                clangd: theirs?.type ?? null,
                cliceAka: ours?.aka ?? null,
                clangdAka: theirs?.aka ?? null,
            });
        }

        // Completions right after a member access the code writes.
        const accesses: number[] = [];
        const calls: number[] = [];
        tokens.forEach((token, index) => {
            const spelling = text.slice(token.begin, token.end);
            const next = tokens[index + 1];
            const previous = tokens[index - 1];
            if (token.directive || token.kind !== "punct") {
                return;
            }
            if ((spelling === "." || spelling === "->") && next?.kind === "ident") {
                accesses.push(token.end);
            }
            if (
                spelling === "(" &&
                previous?.kind === "ident" &&
                !KEYWORDS.has(text.slice(previous.begin, previous.end)) &&
                text.slice(token.end, token.end + 1) !== ")"
            ) {
                calls.push(token.end);
            }
        });
        for (const offset of sample(accesses, opts.completions)) {
            const position = lines.position(offset);
            const replies = await Promise.all(
                servers.map((server, index) =>
                    ask(server, `completion ${at(offset)}`, () =>
                        server.client.completionAt(uri(index), position.line, position.character, {
                            triggerCharacter: text[offset - 1] === "." ? "." : ">",
                        }),
                    ),
                ),
            );
            const members = new Map<string, (string | null)[]>();
            replies.forEach((reply, index) => {
                const server = servers[index];
                const items = Array.isArray(reply) ? reply : (reply?.items ?? []);
                for (const item of items) {
                    if (server === undefined || item.kind === proto.CompletionItemKind.Keyword) {
                        continue;
                    }
                    const member = itemName(item);
                    const types = members.get(member) ?? servers.map(() => null);
                    types[index] ??= itemType(server, item);
                    members.set(member, types);
                }
            });
            for (const [member, types] of members) {
                shown.push({
                    feature: "completion",
                    at: at(offset),
                    snippet: snippet(lines, { begin: offset, end: offset }),
                    subject: member,
                    clice: types[0] ?? null,
                    clangd: types[1] ?? null,
                });
            }
        }

        // The signature help of a call the code writes, its first argument.
        for (const offset of sample(calls, opts.signatures)) {
            const position = lines.position(offset);
            const replies = await Promise.all(
                servers.map((server, index) =>
                    ask(server, `signatureHelp ${at(offset)}`, () =>
                        server.client.signatureHelpAt(
                            uri(index),
                            position.line,
                            position.character,
                        ),
                    ),
                ),
            );
            const labels = replies.map((reply) => {
                const signature = reply?.signatures[reply.activeSignature ?? 0];
                return signature?.label ?? null;
            });
            shown.push({
                feature: "signature",
                at: at(offset),
                snippet: snippet(lines, { begin: offset, end: offset }),
                subject: /\w+$/.exec(text.slice(Math.max(0, offset - 65), offset - 1))?.[0] ?? "",
                clice: labels[0] ?? null,
                clangd: labels[1] ?? null,
            });
        }
        for (const [index, server] of servers.entries()) {
            server.client.close(uri(index));
        }
    }
    for (const server of servers) {
        await stop(server);
    }

    // Spellings by kind, per feature and server; clice's beside clangd's.
    const byKind = new Groups<Example>();
    const differences = new Groups<Example>();
    const counts = new Map<string, number>();
    for (const entry of shown) {
        const example = (detail?: string) => () => ({
            at: entry.at,
            snippet: entry.snippet,
            ...(detail === undefined ? {} : { detail }),
        });
        for (const [server, spelled] of [
            ["clice", entry.clice],
            ["clangd", entry.clangd],
        ] as const) {
            if (spelled === null) {
                continue;
            }
            const key = `${entry.feature} ${server}`;
            counts.set(key, (counts.get(key) ?? 0) + 1);
            for (const kind of kinds(spelled)) {
                byKind.add(`${key}: ${kind}`, `${entry.at} ${entry.subject}`, example(spelled));
            }
        }
        if (opts.clangd !== null && entry.clice !== entry.clangd) {
            const tags = (spelled: string | null) =>
                spelled === null ? "none" : kinds(spelled).join(", ");
            differences.add(
                `${entry.feature}: clice [${tags(entry.clice)}] vs clangd [${tags(entry.clangd)}]`,
                `${entry.at} ${entry.subject}`,
                example(`${entry.clice ?? "∅"}  |  ${entry.clangd ?? "∅"}`),
            );
        }
    }
    const print = (title: string, groups: Groups<Example>) => {
        console.log(`\n## ${title}\n`);
        for (const [key, group] of groups.sorted()) {
            console.log(`- ${group.count} × ${key}`);
            for (const e of group.examples.slice(0, 6)) {
                console.log(
                    `    ${e.at}  ${e.snippet}${e.detail === undefined ? "" : `  [${e.detail}]`}`,
                );
            }
        }
    };
    console.log(`# Type display check: ${opts.files.length} files\n`);
    console.log(
        [...counts]
            .sort()
            .map(([key, count]) => `${key} ${count}`)
            .join(", "),
    );
    print("By kind of spelling", byKind);
    if (opts.clangd !== null) {
        print("clice vs clangd", differences);
    }
    if (failures.length > 0) {
        console.log(`\n## Failures\n`);
        for (const failure of failures.slice(0, 50)) {
            console.log(`- ${failure}`);
        }
    }
    if (opts.json !== null) {
        fs.writeFileSync(opts.json, JSON.stringify({ shown, failures }, null, 1));
    }
}

await main();
