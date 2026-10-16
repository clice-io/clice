/// Selection range check: ask clice — and, given a binary, clangd — for the
/// selection ranges at every token boundary (and some blank space) of real
/// files, check the invariants of every chain and diff the two servers'
/// chains, grouped by the shape of the step that differs.
///
/// Usage:
///   node tools/bench/selection_check.ts --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>         clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>        clangd executable; no diff without it
///   --crlf                 send every file with CRLF line endings
///   --readonly             serve clice's files read-only (the text-only steps)
///   --max-positions <N>    positions per file, sampled evenly beyond (default 4000)
///   --batch <N>            positions per request (default 64)
///   --json <path>          write every finding as JSON
///   --at <line:column>     print both chains at these 1-based positions
///                          instead of checking every position; repeatable
///   --markers              print both chains at each `§` of the files,
///                          which are sent without them
///
/// The CDB directory is the workspace of both servers and must hold a
/// compile_commands.json naming every file.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { CliceClient } from "../client/client.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";

type TokenKind = "ident" | "number" | "string" | "char" | "comment" | "punct" | "header";

interface Token {
    kind: TokenKind;
    begin: number;
    end: number;
}

/// UTF-16 offsets, as the protocol counts them.
interface Span {
    begin: number;
    end: number;
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

const KEYWORDS = new Set(
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
function lex(text: string): Token[] {
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
        tokens.push({ kind, begin: start, end: i });
    }
    return tokens;
}

/// Line starts of `text`, a line ending at `\n` (its `\r` before it
/// belongs to the break, not the line).
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

/// The places a cursor is put: every token's start and end, the middle of
/// a long one, and the blank lines and indentation between them.
function cursorOffsets(text: string, tokens: Token[], lines: Lines): number[] {
    const set = new Set<number>();
    for (const token of tokens) {
        set.add(token.begin);
        set.add(token.end);
        if (token.end - token.begin > 2) {
            set.add((token.begin + token.end) >> 1);
        }
    }
    for (let line = 0; line < lines.starts.length; line += 1) {
        const begin = lines.starts[line] ?? 0;
        set.add(begin);
        set.add(begin + lines.length(line));
    }
    set.add(text.length);
    // No position falls between the two characters of a CRLF break.
    return [...set]
        .filter((offset) => !(text[offset - 1] === "\r" && text[offset] === "\n"))
        .sort((a, b) => a - b);
}

function sample<T>(values: T[], limit: number): T[] {
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

/// Where each offset sits relative to the tokens.
class Layout {
    private readonly starts = new Set<number>();
    private readonly ends = new Set<number>();
    readonly tokens: Token[];

    constructor(tokens: Token[]) {
        this.tokens = tokens;
        for (const token of tokens) {
            this.starts.add(token.begin);
            this.ends.add(token.end);
        }
    }

    /// The token strictly containing `offset`, if any.
    inside(offset: number): Token | undefined {
        let low = 0;
        let high = this.tokens.length - 1;
        while (low <= high) {
            const mid = (low + high) >> 1;
            const token = this.tokens[mid];
            if (token === undefined) {
                break;
            }
            if (token.end <= offset) {
                low = mid + 1;
            } else if (token.begin >= offset) {
                high = mid - 1;
            } else {
                return token;
            }
        }
        return undefined;
    }

    /// The index of the first token ending after `offset`.
    after(offset: number): number {
        let low = 0;
        let high = this.tokens.length;
        while (low < high) {
            const mid = (low + high) >> 1;
            if ((this.tokens[mid]?.end ?? 0) <= offset) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    }

    isStart(offset: number): boolean {
        return this.starts.has(offset);
    }

    isEnd(offset: number): boolean {
        return this.ends.has(offset);
    }
}

const TEXT_KINDS: TokenKind[] = ["string", "char", "comment", "header"];

/// Why an edge of a step is misplaced, if it is: inside a name, a number
/// or a punctuator (`>>` closing two template lists aside), or in the blank
/// space beside the tokens.
function edgeProblem(
    text: string,
    layout: Layout,
    offset: number,
    side: "begin" | "end",
): string | null {
    const token = layout.inside(offset);
    if (token !== undefined) {
        if (TEXT_KINDS.includes(token.kind)) {
            return null;
        }
        const spelling = text.slice(token.begin, token.end);
        if (spelling === ">>" || spelling === ">>=") {
            return null;
        }
        return `${side} inside ${token.kind} \`${spelling}\``;
    }
    if (side === "begin" ? layout.isStart(offset) : layout.isEnd(offset)) {
        return null;
    }
    return `${side} in blank space`;
}

function normalized(text: string, token: Token): string {
    const spelling = text.slice(token.begin, token.end);
    switch (token.kind) {
        case "ident":
            return KEYWORDS.has(spelling) ? spelling : "ID";
        case "number":
            return "NUM";
        case "string":
            return "STR";
        case "char":
            return "CHR";
        case "comment":
            return spelling.startsWith("//") ? "//" : "/*";
        case "header":
            return "<HDR>";
        case "punct":
            return spelling;
    }
}

const CLOSERS: Record<string, string> = { "(": ")", "[": "]", "{": "}", "<": ">" };

/// A step's shape: the inside of which brackets, the inside of which
/// token, or its first and last tokens, names and literals generalized.
function shape(text: string, layout: Layout, span: Span): string {
    if (span.begin === span.end) {
        return "empty";
    }
    const holder = layout.inside(span.begin);
    if (holder !== undefined && holder.end >= span.end && TEXT_KINDS.includes(holder.kind)) {
        return `text of ${holder.kind}`;
    }
    const first = layout.tokens[layout.after(span.begin)];
    const lastIndex = layout.after(span.end - 1);
    const last = layout.tokens[lastIndex];
    if (first === undefined || last === undefined) {
        return "?";
    }
    const before = layout.tokens[layout.after(span.begin) - 1];
    const next = layout.tokens[lastIndex + 1];
    let inner = "";
    if (before !== undefined && next !== undefined) {
        const opener = text.slice(before.begin, before.end);
        const closer = text.slice(next.begin, next.end);
        if (CLOSERS[opener] !== undefined && closer.startsWith(CLOSERS[opener] ?? "")) {
            inner = `inside ${opener}${CLOSERS[opener] ?? ""}: `;
        }
    }
    const head = normalized(text, first);
    const tail = normalized(text, last);
    return first === last ? `${inner}${head}` : `${inner}${head} … ${tail}`;
}

/// One line around a step, the step marked: `a ⟦b⟧ c`, a step over
/// several lines shown from its first line to its last.
function snippet(text: string, lines: Lines, span: Span): string {
    const from = lines.position(span.begin);
    const to = lines.position(span.end);
    const lineText = (line: number) => {
        const begin = lines.starts[line] ?? 0;
        return text.slice(begin, begin + lines.length(line));
    };
    const first = lineText(from.line);
    if (from.line === to.line) {
        return (
            first.slice(0, from.character) +
            "⟦" +
            first.slice(from.character, to.character) +
            "⟧" +
            first.slice(to.character)
        ).trim();
    }
    const last = lineText(to.line);
    return (
        `${first.slice(0, from.character)}⟦${first.slice(from.character)}`.trim() +
        " … " +
        `${last.slice(0, to.character)}⟧${last.slice(to.character)}`.trim() +
        ` (${to.line - from.line + 1} lines)`
    );
}

interface Example {
    at: string;
    step: string;
    cursor?: string;
}

/// Findings of one kind, counted once per file and step.
class Groups {
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
            const file = added.at.replace(/:\d+:\d+$/, "");
            if (limit === 4 || !group.examples.some((e) => e.at.startsWith(file))) {
                group.examples.push(added);
            }
        }
    }

    sorted(): [string, { count: number; examples: Example[] }][] {
        return [...this.groups].sort((a, b) => b[1].count - a[1].count);
    }
}

function flatten(range: proto.SelectionRange | undefined): proto.Range[] {
    const chain: proto.Range[] = [];
    for (let node = range; node !== undefined; node = node.parent) {
        chain.push(node.range);
    }
    return chain;
}

interface Server {
    name: "clice" | "clangd";
    client: CliceClient;
    /// Per position, milliseconds of the request it travelled in, divided.
    millis: number[];
    slowest: number;
}

let readonly = false;

async function start(name: "clice" | "clangd", binary: string, workspace: string): Promise<Server> {
    const client = CliceClient.start(binary, {
        args:
            name === "clice"
                ? ["serve"]
                : [`--compile-commands-dir=${workspace}`, "--background-index=false"],
        cwd: workspace,
    });
    await client.initialize(new Workspace(workspace), {
        testDefaults: false,
        initializationOptions:
            name === "clice"
                ? {
                      rules: [{ compile_commands: [workspace] }],
                      project: {
                          enable_indexing: false,
                          stateful_worker_count: 1,
                          readonly: readonly ? "on" : "off",
                      },
                  }
                : {},
    });
    return { name, client, millis: [], slowest: 0 };
}

async function stop(server: Server): Promise<void> {
    if (server.name === "clice") {
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

interface Options {
    clice: string;
    clangd: string | null;
    cdb: string;
    crlf: boolean;
    readonly: boolean;
    maxPositions: number;
    batch: number;
    json: string | null;
    at: string[];
    markers: boolean;
    files: string[];
}

function options(): Options {
    const { values, positionals } = parseArgs({
        allowPositionals: true,
        options: {
            clice: { type: "string" },
            clangd: { type: "string" },
            cdb: { type: "string" },
            crlf: { type: "boolean", default: false },
            readonly: { type: "boolean", default: false },
            "max-positions": { type: "string", default: "4000" },
            batch: { type: "string", default: "64" },
            json: { type: "string" },
            at: { type: "string", multiple: true },
            markers: { type: "boolean", default: false },
        },
    });
    if (values.cdb === undefined || positionals.length === 0) {
        console.error("usage: node tools/bench/selection_check.ts --cdb <dir> [options] <file>...");
        process.exit(1);
    }
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        clangd: values.clangd ?? null,
        cdb: path.resolve(values.cdb),
        crlf: values.crlf,
        readonly: values.readonly,
        maxPositions: Number(values["max-positions"]),
        batch: Number(values.batch),
        json: values.json ?? null,
        at: values.at ?? [],
        markers: values.markers,
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    readonly = opts.readonly;
    const invariants = new Groups();
    const differences = new Groups();
    const failures: string[] = [];
    const broken: string[] = [];
    let positionCount = 0;
    let compared = 0;
    let agreed = 0;

    let clice = await start("clice", opts.clice, opts.cdb);
    let clangd = opts.clangd === null ? null : await start("clangd", opts.clangd, opts.cdb);

    for (const file of opts.files) {
        // Editors hold a document without its byte order mark.
        let text = fs.readFileSync(file, "utf8").replace(/^\uFEFF/, "");
        if (opts.crlf) {
            text = text.replace(/\r?\n/g, "\r\n");
        }
        const marked: number[] = [];
        if (opts.markers) {
            text = text.replace(/§/g, (_match, offset: number) => {
                marked.push(offset - marked.length);
                return "";
            });
        }
        const tokens = lex(text);
        const lines = new Lines(text);
        const layout = new Layout(tokens);
        const offsets = opts.markers
            ? marked
            : opts.at.length > 0
              ? opts.at.map((spec) => {
                    const [line, column] = spec.split(":").map(Number);
                    return (lines.starts[(line ?? 1) - 1] ?? 0) + (column ?? 1) - 1;
                })
              : sample(cursorOffsets(text, tokens, lines), opts.maxPositions);
        const name = path.relative(opts.cdb, file);
        const at = (offset: number) => {
            const p = lines.position(offset);
            return `${name}:${p.line + 1}:${p.character + 1}`;
        };
        const servers = clangd === null ? [clice] : [clice, clangd];
        const restart = async (server: Server) => {
            server.client.disposed = true;
            server.client.child.kill("SIGKILL");
            const binary = server.name === "clice" ? opts.clice : (opts.clangd ?? "");
            const next = await start(server.name, binary, opts.cdb);
            next.millis = server.millis;
            next.slowest = server.slowest;
            if (server.name === "clice") {
                clice = next;
            } else {
                clangd = next;
            }
        };
        const opened = await Promise.allSettled(
            servers.map(async (server) => {
                if (readonly && server.name === "clice") {
                    // Read-only serving compiles nothing, so no diagnostics arrive.
                    const [uri] = server.client.open(file, 0, { text });
                    await server.client.hoverAt(uri, 0, 0);
                    return uri;
                }
                const [uri] = await withTimeout(
                    server.client.openAndWait(file, 120_000, { text }),
                    120_000,
                    `${server.name} open`,
                );
                return uri;
            }),
        );
        const uris: string[] = [];
        let unopened = false;
        for (const [index, result] of opened.entries()) {
            const server = servers[index];
            if (result.status === "fulfilled") {
                uris.push(result.value);
            } else if (server !== undefined) {
                unopened = true;
                failures.push(`${server.name} ${name}: open failed: ${String(result.reason)}`);
                await restart(server);
            }
        }
        if (unopened) {
            continue;
        }
        const started = Date.now();
        servers.forEach((server, index) => {
            const errors = (server.client.diagnostics.get(uris[index] ?? "") ?? []).filter(
                (diagnostic) => diagnostic.severity === proto.DiagnosticSeverity.Error,
            );
            if (errors.length > 0) {
                const first = errors[0]?.message ?? "";
                const message = typeof first === "string" ? first : first.value;
                broken.push(`${server.name} ${name}: ${errors.length} errors, first: ${message}`);
            }
        });

        const stalled: Server[] = [];
        for (let first = 0; first < offsets.length; first += opts.batch) {
            const batch = offsets.slice(first, first + opts.batch);
            const positions = batch.map((offset) => lines.position(offset));
            const replies = await Promise.all(
                servers.map(async (server, index) => {
                    const begin = performance.now();
                    try {
                        const reply = await withTimeout(
                            server.client.selectionRanges(uris[index] ?? "", positions),
                            120_000,
                            `${server.name} selectionRange`,
                        );
                        const elapsed = performance.now() - begin;
                        server.slowest = Math.max(server.slowest, elapsed);
                        server.millis.push(elapsed / batch.length);
                        return reply;
                    } catch (error) {
                        failures.push(
                            `${server.name} ${at(batch[0] ?? 0)}+${batch.length}: ${String(error)}`,
                        );
                        stalled.push(server);
                        return null;
                    }
                }),
            );
            if (stalled.length > 0) {
                for (const server of stalled.splice(0)) {
                    await restart(server);
                }
                break;
            }
            const [ours, theirs] = replies;
            if (opts.at.length > 0 || opts.markers) {
                batch.forEach((offset, i) => {
                    console.log(
                        `\n${at(offset)}  ${snippet(text, lines, { begin: offset, end: offset }).replace("⟦⟧", "|")}`,
                    );
                    replies.forEach((reply, k) => {
                        console.log(`  ${servers[k]?.name ?? ""}:`);
                        for (const range of flatten(reply?.[i])) {
                            const begin = lines.offset(range.start);
                            const end = lines.offset(range.end);
                            console.log(
                                typeof begin === "number" && typeof end === "number"
                                    ? `    ${snippet(text, lines, { begin, end })}`
                                    : `    invalid ${JSON.stringify(range)}`,
                            );
                        }
                    });
                });
                continue;
            }
            if (ours === null || ours === undefined) {
                continue;
            }
            if (ours.length !== batch.length) {
                failures.push(
                    `clice ${at(batch[0] ?? 0)}: ${ours.length} answers for ${batch.length} positions`,
                );
                continue;
            }
            batch.forEach((offset, i) => {
                positionCount += 1;
                const cursor = at(offset);
                const chain: Span[] = [];
                for (const range of flatten(ours[i])) {
                    const begin = lines.offset(range.start);
                    const end = lines.offset(range.end);
                    if (typeof begin === "string" || typeof end === "string") {
                        invariants.add(
                            `invalid position: ${typeof begin === "string" ? begin : end}`,
                            cursor,
                            () => ({ at: cursor, step: JSON.stringify(range) }),
                        );
                        return;
                    }
                    chain.push({ begin, end });
                }
                const head = chain[0];
                if (head === undefined) {
                    invariants.add("no range at all", cursor, () => ({ at: cursor, step: "" }));
                    return;
                }
                if (head.begin > offset || head.end < offset) {
                    invariants.add(
                        "first step misses the cursor",
                        `${head.begin}-${head.end}`,
                        () => ({
                            at: cursor,
                            step: snippet(text, lines, head),
                        }),
                    );
                }
                chain.forEach((span, k) => {
                    const child = chain[k - 1];
                    if (
                        child !== undefined &&
                        !(
                            span.begin <= child.begin &&
                            span.end >= child.end &&
                            (span.begin < child.begin || span.end > child.end)
                        )
                    ) {
                        invariants.add(
                            "parent does not strictly contain its child",
                            `${child.begin}-${child.end}-${span.begin}-${span.end}`,
                            () => ({
                                at: cursor,
                                step: `${snippet(text, lines, child)}  ⊄  ${snippet(text, lines, span)}`,
                            }),
                        );
                    }
                    if (span.begin === span.end) {
                        return;
                    }
                    for (const side of ["begin", "end"] as const) {
                        const problem = edgeProblem(
                            text,
                            layout,
                            side === "begin" ? span.begin : span.end,
                            side,
                        );
                        if (problem !== null) {
                            invariants.add(
                                `${problem} (${shape(text, layout, span)})`,
                                `${span.begin}-${span.end}`,
                                () => ({
                                    at: cursor,
                                    step: snippet(text, lines, span),
                                }),
                            );
                        }
                    }
                });
                const single = chain.length === 1 && head.begin === head.end;
                const token =
                    layout.inside(offset) ??
                    tokens.find((t) => t.begin === offset || t.end === offset);
                if (single && token !== undefined && token.kind !== "comment") {
                    invariants.add(`only an empty range beside a ${token.kind}`, cursor, () => ({
                        at: cursor,
                        step: snippet(text, lines, token),
                    }));
                }

                if (theirs === null || theirs === undefined) {
                    return;
                }
                const other: Span[] = [];
                for (const range of flatten(theirs[i])) {
                    const begin = lines.offset(range.start);
                    const end = lines.offset(range.end);
                    if (typeof begin === "number" && typeof end === "number") {
                        other.push({ begin, end });
                    }
                }
                compared += 1;
                const key = (s: Span) => `${s.begin}-${s.end}`;
                const mine = new Set(chain.map(key));
                const yours = new Set(other.map(key));
                const missing = other.filter((s) => !mine.has(key(s)) && s.begin !== s.end);
                const extra = chain.filter((s) => !yours.has(key(s)) && s.begin !== s.end);
                if (missing.length === 0 && extra.length === 0) {
                    agreed += 1;
                }
                for (const s of missing) {
                    differences.add(
                        `clangd only: ${shape(text, layout, s)}`,
                        `${name}:${key(s)}`,
                        () => ({
                            at: cursor,
                            step: snippet(text, lines, s),
                            cursor: snippet(text, lines, { begin: offset, end: offset }),
                        }),
                    );
                }
                for (const s of extra) {
                    differences.add(
                        `clice only: ${shape(text, layout, s)}`,
                        `${name}:${key(s)}`,
                        () => ({
                            at: cursor,
                            step: snippet(text, lines, s),
                            cursor: snippet(text, lines, { begin: offset, end: offset }),
                        }),
                    );
                }
            });
        }
        const seconds = (Date.now() - started) / 1000;
        console.error(
            `${name}: ${offsets.length} positions, ${tokens.length} tokens, ${seconds.toFixed(1)}s`,
        );
        servers.forEach((server, index) => {
            if (!server.client.disposed) {
                server.client.close(uris[index] ?? "");
            }
        });
    }

    const percentile = (values: number[], p: number) => {
        const sorted = [...values].sort((a, b) => a - b);
        return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] ?? 0;
    };
    const report = {
        positions: positionCount,
        compared,
        agreed,
        timing: [clice, clangd]
            .filter((s) => s !== null)
            .map((s) => ({
                server: s.name,
                p50_ms_per_position: percentile(s.millis, 0.5),
                p99_ms_per_position: percentile(s.millis, 0.99),
                slowest_request_ms: s.slowest,
            })),
        failures,
        broken,
        invariants: Object.fromEntries(invariants.sorted()),
        differences: Object.fromEntries(differences.sorted()),
    };
    if (opts.json !== null) {
        fs.writeFileSync(opts.json, JSON.stringify(report, null, 2));
    }
    console.log(`positions ${positionCount}, compared ${compared}, identical chains ${agreed}`);
    console.log(JSON.stringify(report.timing));
    for (const failure of failures.slice(0, 20)) {
        console.log(`FAILURE ${failure}`);
    }
    console.log(`${broken.length} file opens with compile errors`);
    for (const line of broken.slice(0, 20)) {
        console.log(`BROKEN ${line}`);
    }
    for (const [title, groups] of [
        ["invariants", invariants],
        ["differences", differences],
    ] as const) {
        console.log(`\n== ${title}`);
        for (const [key, group] of groups.sorted().slice(0, 60)) {
            console.log(`${String(group.count).padStart(6)}  ${key}`);
            for (const example of group.examples.slice(0, 2)) {
                console.log(`          ${example.at}  ${example.step}`);
            }
        }
    }

    try {
        await stop(clice);
    } finally {
        if (clangd !== null) {
            await stop(clangd);
        }
    }
}

await main();
