/// What the checks of one LSP feature over real files share: the lines of a
/// document, findings grouped by kind, and the servers they ask, clice and
/// clangd started the same way.

import * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";

export type TokenKind = "ident" | "number" | "string" | "char" | "comment" | "punct" | "header";

export interface Token {
    kind: TokenKind;
    begin: number;
    end: number;
    /// It is part of a directive line.
    directive: boolean;
}

const PUNCTUATORS = [
    "%:%:",
    "...",
    "<=>",
    "->*",
    "<<=",
    ">>=",
    "::",
    "->",
    ".*",
    "++",
    "--",
    "<<",
    ">>",
    "<=",
    ">=",
    "==",
    "!=",
    "&&",
    "||",
    "+=",
    "-=",
    "*=",
    "/=",
    "%=",
    "&=",
    "|=",
    "^=",
    "##",
    "<:",
    ":>",
    "<%",
    "%>",
    "%:",
];

const ENCODING_PREFIXES = new Set(["u8", "u", "U", "L", "R", "u8R", "uR", "UR", "LR"]);

export const KEYWORDS = new Set(
    (
        "alignas alignof asm auto bool break case catch char char8_t char16_t char32_t class " +
        "concept const consteval constexpr constinit const_cast continue co_await co_return " +
        "co_yield decltype default delete do double dynamic_cast else enum explicit export " +
        "extern false float for friend goto if inline int long mutable namespace new noexcept " +
        "nullptr operator private protected public register reinterpret_cast requires return " +
        "short signed sizeof static static_assert static_cast struct switch template this " +
        "thread_local throw true try typedef typeid typename union unsigned using virtual void " +
        "volatile wchar_t while define include include_next ifdef ifndef elif endif undef pragma"
    ).split(" "),
);

function isIdentChar(c: number): boolean {
    return (
        (c >= 0x30 && c <= 0x39) ||
        (c >= 0x41 && c <= 0x5a) ||
        (c >= 0x61 && c <= 0x7a) ||
        c === 0x5f ||
        c === 0x24 ||
        c >= 0x80
    );
}

function isSpace(c: number): boolean {
    return c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d || c === 0x0b || c === 0x0c;
}

/// A raw lex, good enough to tell token boundaries apart from the inside
/// of a token: comments, literals with prefixes, raw strings and suffixes,
/// header names, pp-numbers and the longest punctuators.
export function lex(text: string): Token[] {
    const tokens: Token[] = [];
    let i = 0;
    let lineStart = true;
    let directive: string | null = null;
    const n = text.length;
    const code = (k: number) => (k < n ? text.charCodeAt(k) : -1);

    const quoted = (start: number, quote: string): number => {
        let k = start;
        while (k < n && text[k] !== quote && text[k] !== "\n") {
            k += text[k] === "\\" ? 2 : 1;
        }
        return Math.min(n, k + 1);
    };
    const suffix = (k: number): number => {
        while (k < n && isIdentChar(code(k))) {
            k += 1;
        }
        return k;
    };

    while (i < n) {
        const c = code(i);
        if (isSpace(c)) {
            if (c === 0x0a) {
                if (
                    directive !== null &&
                    i > 0 &&
                    text[i - 1] !== "\\" &&
                    text.slice(i - 2, i) !== "\\\r"
                ) {
                    directive = null;
                }
                lineStart = true;
            }
            i += 1;
            continue;
        }
        const start = i;
        let kind: TokenKind;
        if (text.startsWith("//", i)) {
            while (i < n && text[i] !== "\n") {
                i += 1;
            }
            if (text[i - 1] === "\r") {
                i -= 1;
            }
            kind = "comment";
        } else if (text.startsWith("/*", i)) {
            const close = text.indexOf("*/", i + 2);
            i = close < 0 ? n : close + 2;
            kind = "comment";
        } else if (c === 0x22 /* " */) {
            i = suffix(quoted(i + 1, '"'));
            kind = "string";
        } else if (c === 0x27 /* ' */) {
            i = suffix(quoted(i + 1, "'"));
            kind = "char";
        } else if (
            c === 0x3c /* < */ &&
            directive !== null &&
            /^(include|include_next|import)$/.test(directive)
        ) {
            const close = text.indexOf(">", i);
            const eol = text.indexOf("\n", i);
            i = close < 0 || (eol >= 0 && eol < close) ? i + 1 : close + 1;
            kind = i - start > 1 ? "header" : "punct";
        } else if (isIdentChar(c) && !(c >= 0x30 && c <= 0x39)) {
            i = suffix(i);
            const word = text.slice(start, i);
            if (
                ENCODING_PREFIXES.has(word) &&
                (text[i] === '"' || (text[i] === "'" && !word.endsWith("R")))
            ) {
                if (word.endsWith("R")) {
                    const open = text.indexOf("(", i);
                    const delimiter = text.slice(i + 1, open);
                    const close = text.indexOf(`)${delimiter}"`, open);
                    i = close < 0 ? n : close + delimiter.length + 2;
                    i = suffix(i);
                } else {
                    i = suffix(quoted(i + 1, text[i] ?? '"'));
                }
                kind = text[start + word.length] === "'" ? "char" : "string";
            } else {
                kind = "ident";
                if (directive === "") {
                    directive = word;
                }
            }
        } else if (
            (c >= 0x30 && c <= 0x39) ||
            (c === 0x2e && code(i + 1) >= 0x30 && code(i + 1) <= 0x39)
        ) {
            i += 1;
            while (i < n) {
                const d = code(i);
                if ((d === 0x2b || d === 0x2d) && /[eEpP]/.test(text[i - 1] ?? "")) {
                    i += 1;
                } else if (d === 0x27 && isIdentChar(code(i + 1))) {
                    i += 2;
                } else if (isIdentChar(d) || d === 0x2e) {
                    i += 1;
                } else {
                    break;
                }
            }
            kind = "number";
        } else {
            // `<::` is `<` `::` unless a `:` or `>` follows ([lex.pptoken]).
            const digraph = text.startsWith("<::", i) && !/[:>]/.test(text[i + 3] ?? "");
            const punctuator = digraph ? "<" : PUNCTUATORS.find((p) => text.startsWith(p, i));
            i += punctuator?.length ?? 1;
            kind = "punct";
            if (c === 0x23 /* # */ && lineStart) {
                directive = "";
            }
        }
        lineStart = false;
        tokens.push({ kind, begin: start, end: i, directive: directive !== null });
    }
    return tokens;
}

/// Line starts of `text`, a line ending at `\n` (its `\r` before it
/// belongs to the break, not the line).
export class Lines {
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

    get count(): number {
        return this.starts.length;
    }

    /// The length of line `line`, its break excluded.
    length(line: number): number {
        const begin = this.starts[line] ?? this.text.length;
        let end =
            line + 1 < this.starts.length ? (this.starts[line + 1] ?? 0) - 1 : this.text.length;
        if (end > begin && this.text[end - 1] === "\r") {
            end -= 1;
        }
        return end - begin;
    }

    /// The text of line `line`, its break excluded.
    lineText(line: number): string {
        const begin = this.starts[line] ?? this.text.length;
        return this.text.slice(begin, begin + this.length(line));
    }

    position(offset: number): proto.Position {
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

    /// The offset of a reply position, or why it is no valid position.
    offset(position: proto.Position): number | string {
        if (position.line < 0 || position.line >= this.starts.length) {
            return `line ${position.line} past the last line`;
        }
        if (position.character < 0 || position.character > this.length(position.line)) {
            return `character ${position.character} past the end of line ${position.line}`;
        }
        const offset = (this.starts[position.line] ?? 0) + position.character;
        const unit = this.text.charCodeAt(offset);
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            return `character ${position.character} splits a surrogate pair`;
        }
        return offset;
    }
}

/// UTF-16 offsets, as the protocol counts them.
export interface Span {
    begin: number;
    end: number;
}

/// At most `limit` of the values, spread evenly over them.
export function sample<T>(values: T[], limit: number): T[] {
    if (values.length <= limit) {
        return values;
    }
    const stride = values.length / limit;
    const picked: T[] = [];
    for (let i = 0; i < limit; i += 1) {
        const value = values[Math.floor(i * stride)];
        if (value !== undefined) {
            picked.push(value);
        }
    }
    return picked;
}

/// One line around a span, the span marked: `a ⟦b⟧ c`, a span over
/// several lines shown from its first line to its last.
export function snippet(lines: Lines, span: Span): string {
    const from = lines.position(span.begin);
    const to = lines.position(span.end);
    const first = lines.lineText(from.line);
    if (from.line === to.line) {
        return (
            first.slice(0, from.character) +
            "⟦" +
            first.slice(from.character, to.character) +
            "⟧" +
            first.slice(to.character)
        ).trim();
    }
    const last = lines.lineText(to.line);
    return (
        `${first.slice(0, from.character)}⟦${first.slice(from.character)}`.trim() +
        " … " +
        `${last.slice(0, to.character)}⟧${last.slice(to.character)}`.trim() +
        ` (${to.line - from.line + 1} lines)`
    );
}

/// Findings of one kind, each counted once per identity; the examples come
/// from different files where they can, a difference from clangd's
/// (`clice only`, `clangd only`) keeping its first few.
export class Groups<Example extends { at: string }> {
    readonly groups = new Map<string, { count: number; examples: Example[] }>();
    private readonly seen = new Set<string>();

    add(key: string, identity: string, example: () => Example): void {
        const limit = key.startsWith("clice only") || key.startsWith("clangd only") ? 4 : 12;
        if (this.seen.has(`${key}\u0000${identity}`)) {
            return;
        }
        this.seen.add(`${key}\u0000${identity}`);
        let group = this.groups.get(key);
        if (group === undefined) {
            group = { count: 0, examples: [] };
            this.groups.set(key, group);
        }
        group.count += 1;
        if (group.examples.length < limit) {
            const added = example();
            const file = added.at.replace(/(:\d+){1,2}$/, "");
            if (limit === 4 || !group.examples.some((e) => e.at.startsWith(file))) {
                group.examples.push(added);
            }
        }
    }

    sorted(): [string, { count: number; examples: Example[] }][] {
        return [...this.groups].sort((a, b) => b[1].count - a[1].count);
    }
}

export interface Server {
    kind: "clice" | "clangd";
    /// How the reports name it.
    name: string;
    client: CliceClient;
    /// Milliseconds per request, or per position a request carried.
    millis: number[];
    slowest: number;
}

export interface ServerOptions {
    name?: string | undefined;
    capabilities?: proto.ClientCapabilities | undefined;
    /// clice serves the files read-only.
    readonly?: boolean | undefined;
    /// More of clice's configuration, as initializationOptions.
    config?: Record<string, unknown> | undefined;
    /// More of clice's `[project]` settings.
    project?: Record<string, unknown> | undefined;
    /// The directory holding the compile_commands.json, the workspace by
    /// default.
    cdb?: string | undefined;
    /// clice's cache directory, `.clice` in the workspace by default.
    cacheDir?: string | undefined;
    /// The server indexes the workspace in the background: clice's
    /// `enable_indexing`, clangd's `--background-index`.
    indexing?: boolean | undefined;
    /// More of clangd's command line.
    args?: string[] | undefined;
}

/// A server on the workspace `workspace`, whose compile_commands.json both
/// servers read; clice with its real defaults, one stateful worker and no
/// indexing.
export async function start(
    kind: "clice" | "clangd",
    binary: string,
    workspace: string,
    options: ServerOptions = {},
): Promise<Server> {
    const cdb = options.cdb ?? workspace;
    const indexing = options.indexing === true;
    const client = CliceClient.start(binary, {
        args:
            kind === "clice"
                ? ["serve"]
                : [
                      `--compile-commands-dir=${cdb}`,
                      `--background-index=${String(indexing)}`,
                      ...(options.args ?? []),
                  ],
        cwd: workspace,
    });
    await client.initialize(new Workspace(workspace), {
        testDefaults: false,
        cacheDir: options.cacheDir,
        capabilities: options.capabilities,
        initializationOptions:
            kind === "clice"
                ? {
                      rules: [{ compile_commands: [cdb] }],
                      project: {
                          enable_indexing: indexing,
                          stateful_worker_count: 1,
                          readonly: options.readonly === true ? "on" : "off",
                          ...options.project,
                      },
                      ...options.config,
                  }
                : {},
    });
    return { kind, name: options.name ?? kind, client, millis: [], slowest: 0 };
}

export async function stop(server: Server): Promise<void> {
    if (server.kind === "clice") {
        await server.client.shutdown();
        return;
    }
    try {
        await withTimeout(
            server.client.sendRequest(proto.ShutdownRequest.type),
            60_000,
            "shutdown",
        );
        await server.client.sendNotification(proto.ExitNotification.type);
    } catch {
        // Killed below.
    }
    server.client.disposed = true;
    // Hang detection: a server still running after `exit` is killed.
    setTimeout(() => server.client.child.kill("SIGKILL"), 5_000).unref();
    await server.client.exited;
}
