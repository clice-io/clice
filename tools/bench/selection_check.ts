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
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    Groups,
    KEYWORDS,
    Lines,
    type Server,
    type Span,
    type Token,
    type TokenKind,
    lex,
    sample,
    snippet,
    start,
    stop,
} from "./lsp_check.ts";

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

interface Example {
    at: string;
    step: string;
    cursor?: string;
}

function flatten(range: proto.SelectionRange | undefined): proto.Range[] {
    const chain: proto.Range[] = [];
    for (let node = range; node !== undefined; node = node.parent) {
        chain.push(node.range);
    }
    return chain;
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
    const invariants = new Groups<Example>();
    const differences = new Groups<Example>();
    const failures: string[] = [];
    const broken: string[] = [];
    let positionCount = 0;
    let compared = 0;
    let agreed = 0;

    const serve = (kind: "clice" | "clangd", binary: string) =>
        start(kind, binary, opts.cdb, { readonly: opts.readonly });
    let clice = await serve("clice", opts.clice);
    let clangd = opts.clangd === null ? null : await serve("clangd", opts.clangd);

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
            const binary = server.kind === "clice" ? opts.clice : (opts.clangd ?? "");
            const next = await serve(server.kind, binary);
            next.millis = server.millis;
            next.slowest = server.slowest;
            if (server.kind === "clice") {
                clice = next;
            } else {
                clangd = next;
            }
        };
        const opened = await Promise.allSettled(
            servers.map(async (server) => {
                if (opts.readonly && server.kind === "clice") {
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
                        `\n${at(offset)}  ${snippet(lines, { begin: offset, end: offset }).replace("⟦⟧", "|")}`,
                    );
                    replies.forEach((reply, k) => {
                        console.log(`  ${servers[k]?.name ?? ""}:`);
                        for (const range of flatten(reply?.[i])) {
                            const begin = lines.offset(range.start);
                            const end = lines.offset(range.end);
                            console.log(
                                typeof begin === "number" && typeof end === "number"
                                    ? `    ${snippet(lines, { begin, end })}`
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
                            step: snippet(lines, head),
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
                                step: `${snippet(lines, child)}  ⊄  ${snippet(lines, span)}`,
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
                                    step: snippet(lines, span),
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
                        step: snippet(lines, token),
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
                            step: snippet(lines, s),
                            cursor: snippet(lines, { begin: offset, end: offset }),
                        }),
                    );
                }
                for (const s of extra) {
                    differences.add(
                        `clice only: ${shape(text, layout, s)}`,
                        `${name}:${key(s)}`,
                        () => ({
                            at: cursor,
                            step: snippet(lines, s),
                            cursor: snippet(lines, { begin: offset, end: offset }),
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
