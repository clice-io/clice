/// Folding range check: ask clice — as a client folding by characters, one
/// folding whole lines as VS Code does, and one also showing collapsed text
/// as Neovim does — and, given a binary, clangd for the folding ranges of
/// real files, check the invariants of every reply, check that the
/// character and whole-line replies agree, and diff clice's whole-line folds
/// against clangd's, grouped by the construct that differs.
///
/// Usage:
///   node tools/bench/folding_check.ts --cdb <dir> [options] <file>...
///
/// Options:
///   --clice <path>     clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>    clangd executable; no diff without it
///   --crlf             send every file with CRLF line endings
///   --json <path>      write every finding as JSON
///   --show             print every fold of every server instead of checking
///   --markers          strip the cases' `§` markers before sending a file
///
/// The CDB directory is the workspace of the servers and must hold a
/// compile_commands.json naming every file.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseArgs } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import { Groups, KEYWORDS, Lines, type Token, lex, snippet, start, stop } from "./lsp_check.ts";

/// VS Code's `editor.foldingMaximumRegions` default.
const VSCODE_REGION_LIMIT = 5000;

/// A request slower than this is a finding.
const SLOW_MS = 1000;

const OPENERS: Record<string, string> = { "(": ")", "[": "]", "{": "}" };

interface Bracket {
    char: string;
    offset: number;
    /// The offset of the token before it, -1 at the start.
    before: number;
}

interface Pair {
    open: Bracket;
    close: number;
}

/// The bracket pairs of the code outside directive lines; null when the
/// brackets do not balance (a conditional whose branches unbalance them).
function bracketPairs(text: string, tokens: Token[]): Pair[] | null {
    const pairs: Pair[] = [];
    const stack: Bracket[] = [];
    let before = -1;
    for (const token of tokens) {
        if (token.kind === "comment" || token.directive) {
            continue;
        }
        const spelling = text.slice(token.begin, token.end);
        if (token.kind === "punct" && OPENERS[spelling] !== undefined) {
            stack.push({ char: spelling, offset: token.begin, before });
        } else if (token.kind === "punct" && /^[)\]}]$/.test(spelling)) {
            const open = stack.pop();
            if (open === undefined || OPENERS[open.char] !== spelling) {
                return null;
            }
            pairs.push({ open, close: token.begin });
        }
        before = token.begin;
    }
    return stack.length === 0 ? pairs : null;
}

/// The lines a whole-line fold hides, shown as its start line and its
/// last hidden line.
function lineSnippet(lines: Lines, start: number, end: number): string {
    const clip = (s: string) => (s.length > 70 ? `${s.slice(0, 67)}...` : s);
    return `${start + 1}: ${clip(lines.lineText(start).trim())} ⟦…⟧ ${end + 1}: ${clip(lines.lineText(end).trim())} | ${end + 2}: ${clip(lines.lineText(end + 1).trim())}`;
}

/// `nvim` folds whole lines and shows collapsed text, as Neovim does.
type Mode = "lines" | "chars" | "nvim";

interface Example {
    at: string;
    text: string;
}

function capabilities(mode: Mode): proto.ClientCapabilities {
    return {
        textDocument: {
            foldingRange: {
                lineFoldingOnly: mode !== "chars",
                foldingRangeKind: { valueSet: ["comment", "imports", "region"] },
                foldingRange: { collapsedText: mode !== "lines" },
            },
        },
    };
}

/// A fold of the characters reply, in offsets.
interface CharFold {
    begin: number;
    end: number;
    kind: string;
    collapsed: string;
}

interface LineFold {
    start: number;
    end: number;
    kind: string;
    collapsed: string;
}

const SECTION_KINDS = new Set([
    "conditionDirective",
    "accessSpecifier",
    "switchCase",
    "region",
    "moduleFragment",
]);

interface Options {
    clice: string;
    clangd: string | null;
    cdb: string;
    crlf: boolean;
    json: string | null;
    show: boolean;
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
            json: { type: "string" },
            show: { type: "boolean", default: false },
            markers: { type: "boolean", default: false },
        },
    });
    if (values.cdb === undefined || positionals.length === 0) {
        console.error("usage: node tools/bench/folding_check.ts --cdb <dir> [options] <file>...");
        process.exit(1);
    }
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        clangd: values.clangd ?? null,
        cdb: path.resolve(values.cdb),
        crlf: values.crlf,
        json: values.json ?? null,
        show: values.show,
        markers: values.markers,
        files: positionals.map((file) => path.resolve(file)),
    };
}

async function main(): Promise<void> {
    const opts = options();
    const invariants = new Groups<Example>();
    const consistency = new Groups<Example>();
    const differences = new Groups<Example>();
    const failures: string[] = [];
    const broken: string[] = [];
    const counts = { files: 0, cliceLines: 0, cliceChars: 0, clangd: 0, same: 0, maxFolds: 0 };
    const kinds = new Map<string, number>();

    const specs: ["clice" | "clangd", Mode][] = [
        ["clice", "lines"],
        ["clice", "chars"],
        ["clice", "nvim"],
    ];
    if (opts.clangd !== null) {
        specs.push(["clangd", "lines"]);
        if (opts.show) {
            specs.push(["clangd", "chars"]);
        }
    }
    const binary = (kind: "clice" | "clangd") =>
        kind === "clice" ? opts.clice : (opts.clangd ?? "");
    const serve = ([kind, mode]: ["clice" | "clangd", Mode]) =>
        start(kind, binary(kind), opts.cdb, {
            name: `${kind}-${mode}`,
            capabilities: capabilities(mode),
        });
    const servers = await Promise.all(specs.map(serve));
    const restart = async (index: number) => {
        const server = servers[index];
        const spec = specs[index];
        if (server === undefined || spec === undefined) {
            return;
        }
        server.client.disposed = true;
        server.client.child.kill("SIGKILL");
        const next = await serve(spec);
        next.millis = server.millis;
        next.slowest = server.slowest;
        servers[index] = next;
    };

    for (const file of opts.files) {
        let text = fs.readFileSync(file, "utf8").replace(/^﻿/, "");
        if (opts.markers) {
            text = text.replace(/§(\([^)]*\))?(⟦|⟧)?/g, "").replace(/⟧/g, "");
        }
        if (opts.crlf) {
            text = text.replace(/\r?\n/g, "\r\n");
        }
        const lines = new Lines(text);
        const name = path.relative(opts.cdb, file);
        const at = (line: number) => `${name}:${line + 1}`;

        const opened = await Promise.allSettled(
            servers.map(async (server) => {
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
            if (result.status === "fulfilled") {
                uris.push(result.value);
            } else {
                unopened = true;
                failures.push(
                    `${servers[index]?.name} ${name}: open failed: ${String(result.reason)}`,
                );
                await restart(index);
            }
        }
        if (unopened) {
            continue;
        }
        const errors = (servers[0]?.client.diagnostics.get(uris[0] ?? "") ?? []).filter(
            (diagnostic) => diagnostic.severity === proto.DiagnosticSeverity.Error,
        );
        if (errors.length > 0) {
            const first = errors[0]?.message ?? "";
            broken.push(
                `${name}: ${errors.length} errors, first: ${typeof first === "string" ? first : first.value}`,
            );
        }

        const replies = await Promise.all(
            servers.map(async (server, index) => {
                const begin = performance.now();
                try {
                    const reply = await withTimeout(
                        server.client.foldingRanges(uris[index] ?? ""),
                        120_000,
                        `${server.name} foldingRange`,
                    );
                    const elapsed = performance.now() - begin;
                    server.millis.push(elapsed);
                    server.slowest = Math.max(server.slowest, elapsed);
                    if (elapsed > SLOW_MS) {
                        failures.push(`${server.name} ${name}: slow, ${elapsed.toFixed(0)} ms`);
                    }
                    return reply ?? [];
                } catch (error) {
                    failures.push(`${server.name} ${name}: ${String(error)}`);
                    await restart(index);
                    return null;
                }
            }),
        );
        servers.forEach((server, index) => {
            if (!server.client.disposed) {
                server.client.close(uris[index] ?? "");
            }
        });
        const [lineReply, charReply, nvimReply, clangdReply] = replies;
        if (
            lineReply === null ||
            lineReply === undefined ||
            charReply === null ||
            charReply === undefined
        ) {
            continue;
        }
        counts.files += 1;

        if (opts.show) {
            for (const [index, reply] of replies.entries()) {
                console.log(`\n== ${servers[index]?.name} ${name}`);
                for (const fold of reply ?? []) {
                    const kind = `${fold.kind ?? "-"}${fold.collapsedText === undefined ? "" : ` ${fold.collapsedText}`}`;
                    if (specs[index]?.[1] === "chars") {
                        const begin = lines.offset({
                            line: fold.startLine,
                            character: fold.startCharacter ?? 0,
                        });
                        const end = lines.offset({
                            line: fold.endLine,
                            character: fold.endCharacter ?? lines.length(fold.endLine),
                        });
                        console.log(
                            typeof begin === "number" && typeof end === "number"
                                ? `  ${kind.padEnd(28)} ${snippet(lines, { begin, end })}`
                                : `  invalid ${JSON.stringify(fold)}`,
                        );
                    } else {
                        console.log(
                            `  ${kind.padEnd(28)} ${lineSnippet(lines, fold.startLine, fold.endLine)}`,
                        );
                    }
                }
            }
            continue;
        }

        // The characters reply.
        const chars: CharFold[] = [];
        for (const fold of charReply) {
            const begin = lines.offset({
                line: fold.startLine,
                character: fold.startCharacter ?? 0,
            });
            const end = lines.offset({ line: fold.endLine, character: fold.endCharacter ?? 0 });
            const kind = fold.kind ?? "(none)";
            if (fold.startCharacter === undefined || fold.endCharacter === undefined) {
                invariants.add(`chars: no character offsets (${kind})`, at(fold.startLine), () => ({
                    at: at(fold.startLine),
                    text: JSON.stringify(fold),
                }));
            }
            if (typeof begin === "string" || typeof end === "string") {
                invariants.add(
                    `chars: invalid position: ${typeof begin === "string" ? begin : end}`,
                    at(fold.startLine),
                    () => ({
                        at: at(fold.startLine),
                        text: JSON.stringify(fold),
                    }),
                );
                continue;
            }
            if (end <= begin) {
                invariants.add(`chars: empty or reversed (${kind})`, at(fold.startLine), () => ({
                    at: at(fold.startLine),
                    text: JSON.stringify(fold),
                }));
                continue;
            }
            if (fold.startLine === fold.endLine) {
                invariants.add(`chars: single line (${kind})`, at(fold.startLine), () => ({
                    at: at(fold.startLine),
                    text: snippet(lines, { begin, end }),
                }));
            }
            chars.push({ begin, end, kind, collapsed: fold.collapsedText ?? "" });
        }
        counts.cliceChars += chars.length;
        for (const fold of chars) {
            kinds.set(fold.kind, (kinds.get(fold.kind) ?? 0) + 1);
        }

        // Edges: a delimited fold spans its delimiters, which its collapsed
        // text repeats; a section runs from a line end to the next header;
        // a run from a line end to a line end.
        const lineEnd = (offset: number) =>
            offset === text.length || text[offset] === "\n" || text.startsWith("\r\n", offset);
        const headerStart = (offset: number) =>
            offset === text.length ||
            text.slice(lines.starts[lines.position(offset).line] ?? 0, offset).trim() === "";
        for (const fold of chars) {
            if (fold.collapsed !== "") {
                const dots = fold.collapsed.indexOf("...");
                const head = fold.collapsed.slice(0, dots);
                const tail = fold.collapsed.slice(dots + 3);
                const closedElsewhere = fold.end === text.length && fold.kind !== "comment";
                if (
                    dots < 0 ||
                    !text.startsWith(head, fold.begin) ||
                    !(text.slice(0, fold.end).endsWith(tail) || closedElsewhere)
                ) {
                    invariants.add(
                        `chars: not on its delimiters (${fold.kind} ${fold.collapsed})`,
                        at(lines.position(fold.begin).line),
                        () => ({
                            at: at(lines.position(fold.begin).line),
                            text: snippet(lines, fold),
                        }),
                    );
                }
            } else if (!lineEnd(fold.begin) && text[fold.begin - 1] !== ":") {
                invariants.add(
                    `chars: section or run starting mid-line (${fold.kind})`,
                    at(lines.position(fold.begin).line),
                    () => ({
                        at: at(lines.position(fold.begin).line),
                        text: snippet(lines, fold),
                    }),
                );
            } else if (SECTION_KINDS.has(fold.kind) ? !headerStart(fold.end) : !lineEnd(fold.end)) {
                invariants.add(
                    `chars: section or run ending mid-line (${fold.kind})`,
                    at(lines.position(fold.begin).line),
                    () => ({
                        at: at(lines.position(fold.begin).line),
                        text: snippet(lines, fold),
                    }),
                );
            }
        }

        // Nesting: two folds nest or are disjoint.
        const sorted = [...chars].sort((a, b) => a.begin - b.begin || b.end - a.end);
        const stack: CharFold[] = [];
        for (const [index, fold] of sorted.entries()) {
            const previous = sorted[index - 1];
            if (previous?.begin === fold.begin && previous.end === fold.end) {
                invariants.add(
                    `chars: duplicate (${previous.kind} / ${fold.kind})`,
                    at(lines.position(fold.begin).line),
                    () => ({
                        at: at(lines.position(fold.begin).line),
                        text: snippet(lines, fold),
                    }),
                );
                continue;
            }
            while (stack.length > 0 && (stack[stack.length - 1]?.end ?? 0) <= fold.begin) {
                stack.pop();
            }
            const top = stack[stack.length - 1];
            if (top !== undefined && fold.end > top.end) {
                invariants.add(
                    `chars: crossing (${top.kind} / ${fold.kind})`,
                    at(lines.position(fold.begin).line),
                    () => ({
                        at: at(lines.position(fold.begin).line),
                        text: `${snippet(lines, top)}  ×  ${snippet(lines, fold)}`,
                    }),
                );
                continue;
            }
            stack.push(fold);
        }

        // A reply to a client folding whole lines: VS Code keeps the first
        // fold starting on a line and drops one crossing the fold it starts
        // in; Neovim nests every fold by its lines.
        const checkLines = (reply: proto.FoldingRange[], label: string): LineFold[] => {
            const folds: LineFold[] = [];
            for (const fold of reply) {
                const kind = fold.kind ?? "(none)";
                if (
                    fold.startLine < 0 ||
                    fold.endLine >= lines.count ||
                    fold.startCharacter !== undefined ||
                    fold.endCharacter !== undefined
                ) {
                    invariants.add(`${label}: invalid (${kind})`, at(fold.startLine), () => ({
                        at: at(fold.startLine),
                        text: JSON.stringify(fold),
                    }));
                    continue;
                }
                if (fold.endLine <= fold.startLine) {
                    invariants.add(`${label}: hides no line (${kind})`, at(fold.startLine), () => ({
                        at: at(fold.startLine),
                        text: lineSnippet(lines, fold.startLine, fold.endLine),
                    }));
                    continue;
                }
                folds.push({
                    start: fold.startLine,
                    end: fold.endLine,
                    kind,
                    collapsed: fold.collapsedText ?? "",
                });
            }
            if (folds.length > VSCODE_REGION_LIMIT) {
                invariants.add(
                    `${label}: more folds than VS Code keeps (${VSCODE_REGION_LIMIT})`,
                    name,
                    () => ({
                        at: name,
                        text: `${folds.length} folds`,
                    }),
                );
            }

            // VS Code keeps the first fold starting on a line, and drops a fold
            // crossing the one it starts in.
            const firstOnLine = new Map<number, LineFold>();
            const seenLines = new Set<string>();
            for (const fold of folds) {
                if (seenLines.has(`${fold.start}-${fold.end}`)) {
                    invariants.add(`${label}: duplicate (${fold.kind})`, at(fold.start), () => ({
                        at: at(fold.start),
                        text: lineSnippet(lines, fold.start, fold.end),
                    }));
                }
                seenLines.add(`${fold.start}-${fold.end}`);
                const first = firstOnLine.get(fold.start);
                if (first === undefined) {
                    firstOnLine.set(fold.start, fold);
                } else if (fold.end > first.end) {
                    invariants.add(
                        `${label}: a shorter fold first on its line (${first.kind} before ${fold.kind})`,
                        at(fold.start),
                        () => ({
                            at: at(fold.start),
                            text: lineSnippet(lines, fold.start, fold.end),
                        }),
                    );
                }
            }
            const kept = [...firstOnLine.values()].sort((a, b) => a.start - b.start);
            const open: LineFold[] = [];
            for (const fold of kept) {
                while (open.length > 0 && (open[open.length - 1]?.end ?? 0) < fold.start) {
                    open.pop();
                }
                const top = open[open.length - 1];
                if (top !== undefined && fold.end > top.end) {
                    invariants.add(
                        `${label}: crossing (${top.kind} / ${fold.kind})`,
                        at(fold.start),
                        () => ({
                            at: at(fold.start),
                            text: `${lineSnippet(lines, top.start, top.end)}  ×  ${lineSnippet(lines, fold.start, fold.end)}`,
                        }),
                    );
                    continue;
                }
                open.push(fold);
            }
            if (label === "nvim") {
                for (const fold of folds) {
                    if (!fold.collapsed.startsWith(lines.lineText(fold.start).trimEnd())) {
                        invariants.add(
                            "nvim: collapsed text is not the first line",
                            at(fold.start),
                            () => ({
                                at: at(fold.start),
                                text: `${lineSnippet(lines, fold.start, fold.end)}  shown as  ${fold.collapsed}`,
                            }),
                        );
                    }
                }
            }
            return folds;
        };
        const folds = checkLines(lineReply, "lines");
        counts.cliceLines += folds.length;
        counts.maxFolds = Math.max(counts.maxFolds, folds.length);
        if (nvimReply !== null && nvimReply !== undefined) {
            checkLines(nvimReply, "nvim");
        }

        // A whole-line fold hides both sides of a bracket pair or neither;
        // only the bracket opening on its start line closes on the line
        // after it, and a conditional's branch may hold half a block.
        const pairs = bracketPairs(text, lex(text));
        if (pairs !== null) {
            for (const fold of folds) {
                if (fold.kind === "conditionDirective") {
                    continue;
                }
                for (const pair of pairs) {
                    const openLine = lines.position(pair.open.offset).line;
                    const closeLine = lines.position(pair.close).line;
                    const hidden = (line: number) => line > fold.start && line <= fold.end;
                    const outside = (line: number) => line < fold.start || line > fold.end + 1;
                    if (
                        (hidden(openLine) && outside(closeLine)) ||
                        (hidden(closeLine) && outside(openLine))
                    ) {
                        invariants.add(
                            `lines: hides one side of a bracket pair (${fold.kind})`,
                            at(fold.start),
                            () => ({
                                at: at(fold.start),
                                text: `${lineSnippet(lines, fold.start, fold.end)}  pair ${openLine + 1}-${closeLine + 1}`,
                            }),
                        );
                        break;
                    }
                }
            }
        }

        // The two clice replies agree: each characters fold hides, in a
        // client folding lines, the lines between its delimiters, or from
        // the end of a section's header line to the line before the next
        // header, or a run's lines below its first.
        const lastTextLine = (() => {
            let line = lines.count - 1;
            while (line > 0 && lines.lineText(line).trim() === "") {
                line -= 1;
            }
            return line;
        })();
        const unmatched = new Set(folds);
        for (const fold of chars) {
            const first = lines.position(fold.begin).line;
            let last: number;
            if (fold.collapsed !== "") {
                const tail = fold.collapsed.slice(fold.collapsed.indexOf("...") + 3);
                // A block an included file closes runs to the end of the
                // file; one a macro closes ends on the macro.
                last =
                    text.slice(0, fold.end).endsWith(tail) && tail !== ""
                        ? lines.position(fold.end - tail.length).line - 1
                        : fold.end === text.length
                          ? lastTextLine
                          : lines.position(fold.end).line - 1;
            } else if (SECTION_KINDS.has(fold.kind)) {
                last = fold.end === text.length ? lastTextLine : lines.position(fold.end).line - 1;
            } else {
                last = lines.position(fold.end).line;
            }
            // Of folds hiding the same lines, the reply keeps one; of folds
            // ending together, the innermost is this one's.
            const match = folds
                .filter((f) => f.end === last && f.start <= first)
                .reduce<
                    LineFold | undefined
                >((best, f) => (best === undefined || f.start > best.start ? f : best), undefined);
            if (last <= first) {
                continue;
            }
            if (match === undefined) {
                consistency.add(`chars fold missing from lines (${fold.kind})`, at(first), () => ({
                    at: at(first),
                    text: `${snippet(lines, fold)}  expected lines ${first + 1}-${last + 1}`,
                }));
            } else {
                unmatched.delete(match);
            }
        }
        for (const fold of unmatched) {
            consistency.add(`lines fold with no chars fold (${fold.kind})`, at(fold.start), () => ({
                at: at(fold.start),
                text: lineSnippet(lines, fold.start, fold.end),
            }));
        }

        // clangd's whole-line folds against clice's.
        if (clangdReply !== null && clangdReply !== undefined) {
            const theirs = clangdReply.filter((f) => f.endLine > f.startLine);
            counts.clangd += theirs.length;
            const key = (start: number, end: number) => `${start}-${end}`;
            const ours = new Set(folds.map((f) => key(f.start, f.end)));
            const oursByEnd = new Map<number, LineFold[]>();
            for (const fold of folds) {
                oursByEnd.set(fold.end, [...(oursByEnd.get(fold.end) ?? []), fold]);
            }
            const matchedOurs = new Set<LineFold>();
            const pairAt = (start: number, end: number) =>
                pairs?.find(
                    (p) =>
                        lines.position(p.open.offset).line === start &&
                        lines.position(p.close).line === end + 1,
                );
            for (const fold of theirs) {
                if (ours.has(key(fold.startLine, fold.endLine))) {
                    counts.same += 1;
                    for (const f of folds) {
                        if (f.start === fold.startLine && f.end === fold.endLine) {
                            matchedOurs.add(f);
                        }
                    }
                    continue;
                }
                const earlier = (oursByEnd.get(fold.endLine) ?? []).find(
                    (f) => f.start < fold.startLine,
                );
                if (earlier !== undefined) {
                    matchedOurs.add(earlier);
                    differences.add(
                        `clice starts earlier (${earlier.kind})`,
                        at(fold.startLine),
                        () => ({
                            at: at(fold.startLine),
                            text: lineSnippet(lines, earlier.start, earlier.end),
                        }),
                    );
                    continue;
                }
                let shape: string;
                if (fold.kind === "comment") {
                    shape = "comment";
                } else {
                    const pair = pairAt(fold.startLine, fold.endLine);
                    if (pair === undefined) {
                        shape = "?";
                    } else {
                        const before =
                            pair.open.before < 0
                                ? "start"
                                : (/^[A-Za-z_]\w*/.exec(text.slice(pair.open.before))?.[0] ??
                                  text[pair.open.before] ??
                                  "");
                        const keyword = KEYWORDS.has(before);
                        const word = /^[A-Za-z_]/.test(before)
                            ? keyword
                                ? before
                                : /^[A-Z][A-Z0-9_]+$/.test(before)
                                  ? "MACRO"
                                  : "ID"
                            : before;
                        shape = `${pair.open.char} after ${word}`;
                    }
                }
                differences.add(`clangd only: ${shape}`, at(fold.startLine), () => ({
                    at: at(fold.startLine),
                    text: lineSnippet(lines, fold.startLine, fold.endLine),
                }));
            }
            for (const fold of folds) {
                if (matchedOurs.has(fold)) {
                    continue;
                }
                differences.add(`clice only: ${fold.kind}`, at(fold.start), () => ({
                    at: at(fold.start),
                    text: lineSnippet(lines, fold.start, fold.end),
                }));
            }
        }
        console.error(`${name}: ${folds.length} line folds, ${chars.length} char folds`);
    }

    const percentile = (values: number[], p: number) => {
        const sorted = [...values].sort((a, b) => a - b);
        return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] ?? 0;
    };
    const report = {
        counts,
        kinds: Object.fromEntries([...kinds].sort((a, b) => b[1] - a[1])),
        timing: servers.map((s) => ({
            server: s.name,
            p50_ms: percentile(s.millis, 0.5),
            p99_ms: percentile(s.millis, 0.99),
            slowest_ms: s.slowest,
        })),
        failures,
        broken,
        invariants: Object.fromEntries(invariants.sorted()),
        consistency: Object.fromEntries(consistency.sorted()),
        differences: Object.fromEntries(differences.sorted()),
    };
    if (opts.json !== null) {
        fs.writeFileSync(opts.json, JSON.stringify(report, null, 2));
    }
    if (!opts.show) {
        console.log(JSON.stringify(counts));
        console.log(JSON.stringify(report.kinds));
        console.log(JSON.stringify(report.timing));
        for (const failure of failures.slice(0, 30)) {
            console.log(`FAILURE ${failure}`);
        }
        console.log(`${broken.length} files with compile errors`);
        for (const line of broken.slice(0, 10)) {
            console.log(`BROKEN ${line}`);
        }
        for (const [title, groups] of [
            ["invariants", invariants],
            ["consistency", consistency],
            ["differences", differences],
        ] as const) {
            console.log(`\n== ${title}`);
            for (const [key, group] of groups.sorted().slice(0, 80)) {
                console.log(`${String(group.count).padStart(6)}  ${key}`);
                for (const example of group.examples.slice(0, 4)) {
                    console.log(`          ${example.at}  ${example.text}`);
                }
            }
        }
    }

    for (const server of servers) {
        await stop(server);
    }
}

await main();
