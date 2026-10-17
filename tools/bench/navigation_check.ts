/// Navigation check: ask every navigation request — definition,
/// declaration, type definition, implementation, references, document
/// highlight, the call and type hierarchies — at every name of real files,
/// through clice's index (a read-only session, the open file served from
/// the index), clice's compiled document, `clice query` and, given a
/// binary, clangd with its
/// background index; diff the answers, grouped by what the symbol is and
/// what the place one side alone gives is. Workspace symbol search is asked
/// for names of the same files, of clice, `clice query symbol-search` and
/// clangd.
///
/// Usage:
///   node tools/bench/navigation_check.ts --workspace <dir> [options] <file>...
///   node tools/bench/navigation_check.ts --cases <prefix> [options]
///
/// Options:
///   --cdb <dir>             directory of compile_commands.json (default: the workspace)
///   --cases <prefix>        every case under tests/<prefix>, each in its own
///                           copy, every input file checked
///   --clice <path>          clice executable (default build/RelWithDebInfo/bin/bin/clice)
///   --clangd <path>         clangd executable; no diff against it without one
///   --cli <N>               also ask `clice query` at N positions of each file
///   --symbols <N>           also search N names of the files (workspace/symbol)
///   --max-positions <N>     positions per file, sampled evenly beyond (default 1500)
///   --jobs <N>              requests in flight per server (default 8)
///   --json <path>           write every finding as JSON
///   --at <file:line:col>    print every answer at these 1-based positions
///                           instead of checking; repeatable
///   --full                  with --at, print every answer whole
///
/// clice indexes the workspace with `clice index` first; clangd builds its
/// background index before the first file is asked.

import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { parseArgs, promisify } from "node:util";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import {
    CASES_DIR,
    MARKED,
    materialize,
    mergeProjects,
    readProject,
    writeDatabases,
} from "../testing/cases/project.ts";
import { withTimeout } from "../promise.ts";
import { REPO_ROOT } from "../repo.ts";
import {
    Groups,
    KEYWORDS,
    Lines,
    type Server,
    type Token,
    lex,
    sample,
    snippet,
    start,
    stop,
} from "./lsp_check.ts";

const run = promisify(execFile);

/// Keywords that navigate somewhere: the type an `auto` stands for, the
/// function an operator spelling calls, the loop a `break` leaves.
const NAVIGABLE_KEYWORDS = new Set([
    "auto",
    "decltype",
    "this",
    "operator",
    "new",
    "delete",
    "override",
    "final",
    "break",
    "continue",
    "co_await",
    "co_return",
    "co_yield",
]);

const REQUESTS = [
    "definition",
    "declaration",
    "typeDefinition",
    "implementation",
    "references",
    "highlight",
    "callItems",
    "incoming",
    "outgoing",
    "typeItems",
    "supertypes",
    "subtypes",
] as const;

type Request = (typeof REQUESTS)[number];

/// Each request's answer as sorted place keys (`file:line:column`, items
/// `file:line`); null when the request failed.
type Answers = Partial<Record<Request, string[] | null>>;

const SYMBOL_KINDS = new Map<number, string>(
    Object.entries(proto.SymbolKind).map(([name, value]) => [value, name]),
);

/// A highlight's kind, as a suffix of its place.
const HIGHLIGHT_KIND = ["", "text", "read", "write"];

interface Options {
    clice: string;
    clangd: string | null;
    workspace: string;
    cdb: string;
    cases: string | null;
    cli: number;
    symbols: number;
    maxPositions: number;
    jobs: number;
    json: string | null;
    at: string[];
    /// With --at, every place of an answer rather than the first few.
    full: boolean;
    files: string[];
}

function options(): Options {
    const { values, positionals } = parseArgs({
        allowPositionals: true,
        options: {
            clice: { type: "string" },
            clangd: { type: "string" },
            workspace: { type: "string" },
            cdb: { type: "string" },
            cases: { type: "string" },
            cli: { type: "string", default: "0" },
            symbols: { type: "string", default: "0" },
            "max-positions": { type: "string", default: "1500" },
            jobs: { type: "string", default: "8" },
            json: { type: "string" },
            at: { type: "string", multiple: true },
            full: { type: "boolean", default: false },
        },
    });
    if (
        values.cases === undefined &&
        (values.workspace === undefined || positionals.length === 0)
    ) {
        console.error(
            "usage: node tools/bench/navigation_check.ts --workspace <dir> [options] <file>...\n" +
                "       node tools/bench/navigation_check.ts --cases <prefix> [options]",
        );
        process.exit(1);
    }
    const workspace = path.resolve(values.workspace ?? ".");
    return {
        clice: path.resolve(
            values.clice ?? path.join(REPO_ROOT, "build/RelWithDebInfo/bin/bin/clice"),
        ),
        clangd: values.clangd ?? null,
        workspace,
        cdb: path.resolve(values.cdb ?? workspace),
        cases: values.cases ?? null,
        cli: Number(values.cli),
        symbols: Number(values.symbols),
        maxPositions: Number(values["max-positions"]),
        jobs: Number(values.jobs),
        json: values.json ?? null,
        at: values.at ?? [],
        full: values.full,
        files: positionals.map((file) => path.resolve(file)),
    };
}

/// Run `work` over `items`, `jobs` at a time.
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

/// The files the answers point into, read once, and how a report names them.
class Files {
    private readonly texts = new Map<string, Lines | null>();
    /// The file each report name stands for.
    private readonly named = new Map<string, string>();
    readonly root: string;

    constructor(root: string) {
        this.root = fs.realpathSync(root);
    }

    real(file: string): string {
        try {
            return fs.realpathSync(file);
        } catch {
            return path.resolve(file);
        }
    }

    /// The name of `file` in a report: workspace-relative, a library's
    /// headers by the library.
    name(file: string): string {
        const real = this.real(file);
        const name = real.startsWith(this.root + path.sep)
            ? path.relative(this.root, real)
            : real
                  .replace(/^.*\/include\/c\+\+\/[^/]+\//, "<libstdc++>/")
                  .replace(/^.*\/lib\/clang\/[^/]+\/include\//, "<clang>/")
                  .replace(/^(.*\/sysroot)?\/usr\/include\/(x86_64-linux-gnu\/)?/, "<libc>/");
        this.named.set(name, real);
        return name;
    }

    /// The file a report name stands for.
    file(name: string): string {
        return this.named.get(name) ?? path.join(this.root, name);
    }

    lines(file: string): Lines | null {
        const real = this.real(file);
        let lines = this.texts.get(real);
        if (lines === undefined) {
            try {
                lines = new Lines(fs.readFileSync(real, "utf8").replace(/^\uFEFF/, ""));
            } catch {
                lines = null;
            }
            this.texts.set(real, lines);
        }
        return lines;
    }
}

interface Place {
    file: string;
    line: number;
    column: number;
}

/// A workspace symbol's name split before the parameter list a function's
/// carries, `operator()(int x) const` into `operator()` and `(int x) const`.
function splitSignature(name: string): [string, string] {
    const close = name.lastIndexOf(")");
    if (close < 0) {
        return [name, ""];
    }
    let depth = 0;
    for (let i = close; i >= 0; i -= 1) {
        if (name[i] === ")") {
            depth += 1;
        } else if (name[i] === "(") {
            depth -= 1;
            if (depth === 0) {
                return [name.slice(0, i), name.slice(i)];
            }
        }
    }
    return [name, ""];
}

function parsePlace(key: string): Place {
    const match = /^(.*):(\d+):(\d+)(?::[a-z]+)?$/.exec(key) ?? /^(.*):(\d+)$/.exec(key);
    return {
        file: match?.[1] ?? key,
        line: Number(match?.[2] ?? 0),
        column: Number(match?.[3] ?? 1),
    };
}

/// What a declaration line declares, by its spelling around `column`.
function classifyLine(text: string, column: number): string {
    const line = text.trim();
    const head = text.slice(0, column - 1);
    const name = /^[A-Za-z_$][\w$]*/.exec(text.slice(column - 1))?.[0] ?? "";
    if (/^#\s*define\b/.test(line)) {
        return "macro";
    }
    if (line.startsWith("#")) {
        return `#${/^#\s*(\w+)/.exec(line)?.[1] ?? ""}`;
    }
    if (/\bnamespace\s+([\w:]+\s*)?$/.test(head) || /\bnamespace\s*$/.test(head)) {
        return "namespace";
    }
    if (/\b(class|struct|union)\s+([\w:]+\s+)*$/.test(head)) {
        const rest = text.slice(column - 1 + name.length);
        return /^\s*;/.test(rest) ? "class forward" : "class";
    }
    if (/\benum\b/.test(head)) {
        return "enum";
    }
    if (/\busing\s+$/.test(head) && /^\s*=/.test(text.slice(column - 1 + name.length))) {
        return "alias";
    }
    if (/\btypedef\b/.test(line)) {
        return "typedef";
    }
    if (/\busing\b/.test(head)) {
        return "using";
    }
    if (/\boperator\b/.test(text) && /operator\s*\S*$/.test(head + name)) {
        return "operator";
    }
    if (/~\s*$/.test(head)) {
        return "destructor";
    }
    const rest = text.slice(column - 1 + name.length);
    if (/^\s*\(/.test(rest)) {
        if (new RegExp(`\\b${name}\\s*::\\s*$`).test(head) || /^\s*(explicit\s+)?$/.test(head)) {
            return "constructor or function";
        }
        return "function";
    }
    if (/^\s*[:;,=)\]{[]/.test(rest) || /^\s*$/.test(rest)) {
        return /\(\s*$|,\s*$|\(\s*[^()]*$/.test(head) ? "parameter or variable" : "variable";
    }
    return "other";
}

interface Site {
    file: string;
    offset: number;
    token: Token;
    /// The token's spelling.
    spelling: string;
    lines: Lines;
    text: string;
}

interface Example {
    at: string;
    detail: string;
}

/// A server, and how the check asks it.
interface Asked {
    server: Server;
    /// Hierarchy expansions already asked, by item.
    expanded: Map<string, string[] | null>;
}

function locationKeys(
    files: Files,
    reply: proto.Location | proto.Location[] | proto.LocationLink[] | null | undefined,
): string[] {
    const list =
        reply === null || reply === undefined ? [] : Array.isArray(reply) ? reply : [reply];
    const keys = list.map((loc) => {
        const uri = "targetUri" in loc ? loc.targetUri : loc.uri;
        const range = "targetUri" in loc ? loc.targetSelectionRange : loc.range;
        const file = URI.parse(uri).fsPath;
        return `${files.name(file)}:${range.start.line + 1}:${range.start.character + 1}`;
    });
    return [...new Set(keys)].sort();
}

function itemKey(files: Files, item: { uri: string; selectionRange: proto.Range }): string {
    return `${files.name(URI.parse(item.uri).fsPath)}:${item.selectionRange.start.line + 1}`;
}

async function safely<T>(promise: Promise<T>): Promise<T | null> {
    try {
        return await withTimeout(promise, 120_000, "request");
    } catch {
        return null;
    }
}

/// Every request at one position.
async function ask(asked: Asked, files: Files, uri: string, at: proto.Position): Promise<Answers> {
    const client = asked.server.client;
    const { line, character } = at;
    const began = performance.now();
    const [definition, declaration, typeDefinition, implementation, references, highlight] =
        await Promise.all([
            safely(client.definitionAt(uri, line, character)),
            safely(client.declarationAt(uri, line, character)),
            safely(client.typeDefinitionAt(uri, line, character)),
            safely(client.implementationAt(uri, line, character)),
            safely(client.referencesAt(uri, line, character, { includeDeclaration: true })),
            safely(client.documentHighlightAt(uri, line, character)),
        ]);
    const [calls, types] = await Promise.all([
        safely(client.prepareCallHierarchy(uri, line, character)),
        safely(client.prepareTypeHierarchy(uri, line, character)),
    ]);
    const elapsed = performance.now() - began;
    asked.server.millis.push(elapsed);
    asked.server.slowest = Math.max(asked.server.slowest, elapsed);

    const expand = async <Item extends { uri: string; selectionRange: proto.Range }>(
        kind: string,
        item: Item | undefined,
        call: (item: Item) => Promise<{ uri: string; selectionRange: proto.Range }[] | null>,
    ): Promise<string[] | null | undefined> => {
        if (item === undefined) {
            return undefined;
        }
        const key = `${kind} ${item.uri} ${JSON.stringify(item.selectionRange)}`;
        if (!asked.expanded.has(key)) {
            const reply = await safely(call(item));
            asked.expanded.set(
                key,
                reply === null ? null : [...new Set(reply.map((r) => itemKey(files, r)))].sort(),
            );
        }
        return asked.expanded.get(key);
    };
    const callItem = calls?.[0];
    const typeItem = types?.[0];
    const [incoming, outgoing, supertypes, subtypes] = await Promise.all([
        expand("in", callItem, async (item) =>
            ((await client.callHierarchyIncoming(item)) ?? []).map((call) => call.from),
        ),
        expand("out", callItem, async (item) =>
            ((await client.callHierarchyOutgoing(item)) ?? []).map((call) => call.to),
        ),
        expand("super", typeItem, async (item) => client.typeHierarchySupertypes(item)),
        expand("sub", typeItem, async (item) => client.typeHierarchySubtypes(item)),
    ]);
    const answers: Answers = {
        definition: definition === null ? null : locationKeys(files, definition),
        declaration: declaration === null ? null : locationKeys(files, declaration),
        typeDefinition: typeDefinition === null ? null : locationKeys(files, typeDefinition),
        implementation: implementation === null ? null : locationKeys(files, implementation),
        references: references === null ? null : locationKeys(files, references),
        highlight:
            highlight === null
                ? null
                : highlight
                      .map(
                          (h) =>
                              `${h.range.start.line + 1}:${h.range.start.character + 1}:${HIGHLIGHT_KIND[h.kind ?? 1] ?? ""}`,
                      )
                      .sort(),
        callItems: calls === null ? null : [...new Set(calls.map((i) => itemKey(files, i)))],
        typeItems: types === null ? null : [...new Set(types.map((i) => itemKey(files, i)))],
    };
    if (incoming !== undefined) {
        answers.incoming = incoming;
    }
    if (outgoing !== undefined) {
        answers.outgoing = outgoing;
    }
    if (supertypes !== undefined) {
        answers.supertypes = supertypes;
    }
    if (subtypes !== undefined) {
        answers.subtypes = subtypes;
    }
    return answers;
}

/// What `clice query` answers at one position, as the editor's keys.
interface CliAnswers {
    definition: { file: string; start: number; end: number } | string;
    references: string[] | string;
    callers: string[] | string;
    callees: string[] | string;
    supertypes: string[] | string;
    subtypes: string[] | string;
}

interface CliEntry {
    file: string;
    line: number;
}

async function cliQuery(
    opts: Options,
    workspace: string,
    args: string[],
): Promise<{ result?: unknown; error?: string }> {
    try {
        const { stdout } = await run(opts.clice, ["query", ...args, "--workspace", workspace], {
            maxBuffer: 1 << 28,
        });
        return JSON.parse(stdout) as { result?: unknown };
    } catch (error) {
        const stdout = (error as { stdout?: string }).stdout ?? "";
        try {
            return JSON.parse(stdout) as { error?: string };
        } catch {
            return { error: `crashed: ${String(error).slice(0, 200)}` };
        }
    }
}

async function askCli(
    opts: Options,
    files: Files,
    workspace: string,
    locator: string,
): Promise<CliAnswers> {
    const entries = (list: CliEntry[] | undefined) =>
        [...new Set((list ?? []).map((e) => `${files.name(e.file)}:${e.line}`))].sort();
    const [definition, references, calls, types] = await Promise.all([
        cliQuery(opts, workspace, ["definition", locator]),
        cliQuery(opts, workspace, ["references", locator, "--include-declaration"]),
        cliQuery(opts, workspace, ["call-graph", locator]),
        cliQuery(opts, workspace, ["type-hierarchy", locator]),
    ]);
    const error = (reply: { error?: string }) =>
        `error: ${(reply.error ?? "no result").replace(/\d+ symbols/, "N symbols")}`;
    const defined = definition.result as
        | { definition?: { file: string; startLine: number; endLine: number } | null }
        | undefined;
    const callGraph = calls.result as { callers?: CliEntry[]; callees?: CliEntry[] } | undefined;
    const hierarchy = types.result as
        | { supertypes?: CliEntry[]; subtypes?: CliEntry[] }
        | undefined;
    const referenced = references.result as { references?: CliEntry[] } | undefined;
    return {
        definition:
            defined === undefined
                ? error(definition)
                : defined.definition === null || defined.definition === undefined
                  ? "no definition"
                  : {
                        file: files.name(defined.definition.file),
                        start: defined.definition.startLine,
                        end: defined.definition.endLine,
                    },
        references: referenced === undefined ? error(references) : entries(referenced.references),
        callers: callGraph === undefined ? error(calls) : entries(callGraph.callers),
        callees: callGraph === undefined ? error(calls) : entries(callGraph.callees),
        supertypes: hierarchy === undefined ? error(types) : entries(hierarchy.supertypes),
        subtypes: hierarchy === undefined ? error(types) : entries(hierarchy.subtypes),
    };
}

/// The places to put a cursor: every name, the keywords that navigate,
/// the headers of include lines.
function cursorTokens(text: string, tokens: Token[]): Token[] {
    return tokens.filter((token) => {
        if (token.kind === "header") {
            return true;
        }
        if (token.kind !== "ident") {
            return false;
        }
        const spelling = text.slice(token.begin, token.end);
        return !KEYWORDS.has(spelling) || NAVIGABLE_KEYWORDS.has(spelling);
    });
}

/// Where the cursor is, by its neighbours.
function cursorClass(site: Site, previous: string, next: string): string {
    if (site.token.kind === "header") {
        return "include header";
    }
    if (KEYWORDS.has(site.spelling)) {
        return `keyword ${site.spelling}`;
    }
    if (site.token.directive) {
        return "in a directive";
    }
    if (previous === "." || previous === "->") {
        return "member access";
    }
    if (previous === "::") {
        return next === "(" ? "qualified call" : "qualified name";
    }
    if (previous === "~") {
        return "destructor name";
    }
    if (next === "(") {
        return "name(";
    }
    if (next === "<") {
        return "name<";
    }
    return "name";
}

interface Report {
    positions: number;
    files: number;
    broken: string[];
    failures: string[];
    timing: Record<string, unknown>[];
    agreements: Record<string, { compared: number; agreed: number }>;
    groups: Record<string, Groups<Example>>;
}

class Checker {
    readonly report: Report;
    private readonly opts: Options;

    constructor(opts: Options) {
        this.opts = opts;
        this.report = {
            positions: 0,
            files: 0,
            broken: [],
            failures: [],
            timing: [],
            agreements: {},
            groups: {
                "index vs ast": new Groups<Example>(),
                "clice vs clangd": new Groups<Example>(),
                "cli vs editor": new Groups<Example>(),
                "workspace symbol": new Groups<Example>(),
                invalid: new Groups<Example>(),
            },
        };
    }

    private group(name: string): Groups<Example> {
        const group = this.report.groups[name];
        if (group === undefined) {
            throw new Error(`no group ${name}`);
        }
        return group;
    }

    private agree(what: string, same: boolean): void {
        const entry = (this.report.agreements[what] ??= { compared: 0, agreed: 0 });
        entry.compared += 1;
        if (same) {
            entry.agreed += 1;
        }
    }

    /// A target's spelling and what its line declares.
    private describe(files: Files, key: string): string {
        const place = parsePlace(key);
        const lines = files.lines(files.file(place.file));
        if (lines === null) {
            return "unreadable";
        }
        const text = lines.lineText(place.line - 1);
        return classifyLine(text, place.column);
    }

    private show(files: Files, key: string): string {
        const place = parsePlace(key);
        const lines = files.lines(files.file(place.file));
        if (lines === null) {
            return key;
        }
        const begin = (lines.starts[place.line - 1] ?? 0) + place.column - 1;
        const word = /^[\w~]*/.exec(lines.text.slice(begin))?.[0].length ?? 0;
        return `${key}  ${snippet(lines, { begin, end: begin + word })}`;
    }

    /// The symbol under the cursor, by what its definition line declares.
    private symbolClass(files: Files, site: Site, answers: Answers[]): string {
        if (KEYWORDS.has(site.spelling)) {
            return `keyword ${site.spelling}`;
        }
        if (site.token.kind === "header") {
            return "include header";
        }
        for (const answer of answers) {
            const target = answer.definition?.[0] ?? answer.declaration?.[0];
            if (target !== undefined) {
                return this.describe(files, target);
            }
        }
        return "unresolved";
    }

    /// Compare two sides' answers at one position.
    private diff(
        groupName: string,
        names: [string, string],
        files: Files,
        site: Site,
        symbol: string,
        cursor: string,
        ours: Answers,
        theirs: Answers,
    ): void {
        const at = `${files.name(site.file)}:${site.lines.position(site.offset).line + 1}:${site.lines.position(site.offset).character + 1}`;
        for (const request of REQUESTS) {
            const mine = ours[request];
            const other = theirs[request];
            if (mine === undefined || other === undefined || mine === null || other === null) {
                continue;
            }
            const same = mine.length === other.length && mine.every((k, i) => k === other[i]);
            this.agree(`${groupName} ${request}`, same);
            if (same) {
                continue;
            }
            const group = this.group(groupName);
            const cursorText = snippet(site.lines, {
                begin: site.token.begin,
                end: site.token.end,
            });
            if (mine.length === 0 || other.length === 0) {
                const empty = mine.length === 0 ? names[0] : names[1];
                const full = mine.length === 0 ? other : mine;
                group.add(`${request}: ${empty} empty | ${symbol} | ${cursor}`, at, () => ({
                    at,
                    detail: `${cursorText}  →  ${full.length}: ${this.show(files, full[0] ?? "")}`,
                }));
                continue;
            }
            if (request === "highlight") {
                const strip = (key: string) => key.replace(/:[a-z]+$/, "");
                const mineRanges = mine.map(strip);
                const otherRanges = other.map(strip);
                const sameRanges =
                    mineRanges.length === otherRanges.length &&
                    mineRanges.every((k, i) => k === otherRanges[i]);
                if (sameRanges) {
                    const kinds = mine
                        .map((k, i) => [k, other[i] ?? ""])
                        .filter(([a, b]) => a !== b)
                        .map(
                            ([a, b]) =>
                                `${/[a-z]+$/.exec(a ?? "")?.[0] ?? ""}/${/[a-z]+$/.exec(b ?? "")?.[0] ?? ""}`,
                        );
                    group.add(
                        `highlight kinds differ ${[...new Set(kinds)].join(",")} (${names.join("/")}) | ${symbol}`,
                        at,
                        () => ({ at, detail: cursorText }),
                    );
                    continue;
                }
            }
            const theirSet = new Set(other);
            const mySet = new Set(mine);
            for (const [side, keys, against] of [
                [names[0], mine, theirSet],
                [names[1], other, mySet],
            ] as const) {
                for (const key of keys) {
                    if (against.has(key)) {
                        continue;
                    }
                    const target =
                        request === "highlight"
                            ? `${files.name(site.file)}:${key.replace(/:[a-z]+$/, "")}`
                            : key;
                    const sameLine = [...against].some(
                        (k) =>
                            k.replace(/:\d+(:[a-z]+)?$/, "") === key.replace(/:\d+(:[a-z]+)?$/, ""),
                    );
                    const kind = sameLine
                        ? "same line, other column"
                        : this.describe(files, target);
                    group.add(
                        `${request}: ${side} only | ${symbol} → ${kind}`,
                        `${request} ${target}`,
                        () => ({
                            at,
                            detail: `${cursorText}  →  ${this.show(files, target)}`,
                        }),
                    );
                }
            }
        }
    }

    private cliDiff(
        files: Files,
        site: Site,
        symbol: string,
        editor: Answers,
        cli: CliAnswers,
    ): void {
        const group = this.group("cli vs editor");
        const position = site.lines.position(site.offset);
        const at = `${files.name(site.file)}:${position.line + 1}:${position.character + 1}`;
        const cursorText = snippet(site.lines, { begin: site.token.begin, end: site.token.end });
        const lines = (keys: string[] | null | undefined) =>
            [...new Set((keys ?? []).map((k) => k.replace(/:\d+$/, "")))].sort();
        const compare = (question: string, ours: string[] | string, theirs: string[]) => {
            if (typeof ours === "string") {
                const same = theirs.length === 0;
                this.agree(`cli ${question}`, same);
                if (!same) {
                    group.add(`${question}: cli ${ours} | ${symbol}`, at, () => ({
                        at,
                        detail: `${cursorText}  editor ${theirs.length}: ${theirs[0] ?? ""}`,
                    }));
                }
                return;
            }
            const same = ours.length === theirs.length && ours.every((k, i) => k === theirs[i]);
            this.agree(`cli ${question}`, same);
            if (same) {
                return;
            }
            const mine = new Set(ours);
            const other = new Set(theirs);
            const cliOnly = ours.filter((k) => !other.has(k));
            const editorOnly = theirs.filter((k) => !mine.has(k));
            const which =
                ours.length === 0
                    ? "cli empty"
                    : theirs.length === 0
                      ? "editor empty"
                      : cliOnly.length > 0 && editorOnly.length > 0
                        ? "both have others"
                        : cliOnly.length > 0
                          ? "cli has more"
                          : "editor has more";
            group.add(`${question}: ${which} | ${symbol}`, at, () => ({
                at,
                detail: `${cursorText}  cli only ${cliOnly.slice(0, 2).join(" ")}  editor only ${editorOnly.slice(0, 2).join(" ")}`,
            }));
        };
        // The CLI's definition is the symbol's definition whichever site the
        // cursor stands on; the editor's alternates with the declarations.
        const definitions = lines(editor.definition);
        const declarations = lines(editor.declaration);
        if (typeof cli.definition === "string") {
            const same = cli.definition === "no definition" && definitions.length === 0;
            this.agree("cli definition", same);
            if (!same) {
                group.add(`definition: cli ${cli.definition} | ${symbol}`, at, () => ({
                    at,
                    detail: `${cursorText}  editor ${definitions.length}: ${definitions[0] ?? ""}`,
                }));
            }
        } else {
            const { file, start, end } = cli.definition;
            const inside = (key: string) => {
                const place = parsePlace(key);
                return place.file === file && place.line >= start && place.line <= end;
            };
            const self = `${files.name(site.file)}:${position.line + 1}`;
            const same = [...definitions, ...declarations, self].some(inside);
            this.agree("cli definition", same);
            if (!same) {
                group.add(`definition: cli elsewhere | ${symbol}`, at, () => ({
                    at,
                    detail: `${cursorText}  cli ${file}:${start}-${end}  editor ${definitions[0] ?? "none"}`,
                }));
            }
        }
        compare("references", cli.references, lines(editor.references));
        compare("callers", cli.callers, editor.incoming ?? []);
        compare("callees", cli.callees, editor.outgoing ?? []);
        compare("supertypes", cli.supertypes, editor.supertypes ?? []);
        compare("subtypes", cli.subtypes, editor.subtypes ?? []);
    }

    /// Check one workspace: `paths` its files to put cursors in.
    async workspace(workspace: string, cdb: string, paths: string[]): Promise<void> {
        const opts = this.opts;
        const files = new Files(workspace);
        const indexStart = performance.now();
        try {
            await run(opts.clice, ["index", "--workspace", workspace], { maxBuffer: 1 << 26 });
        } catch (error) {
            this.report.failures.push(`clice index ${workspace}: ${String(error).slice(0, 300)}`);
        }
        console.error(
            `${files.name(workspace)}: clice index ${((performance.now() - indexStart) / 1000).toFixed(1)}s`,
        );
        const capabilities: proto.ClientCapabilities = {
            window: { workDoneProgress: true },
            textDocument: {
                hover: { contentFormat: ["markdown", "plaintext"] },
                publishDiagnostics: { relatedInformation: true },
            },
        };
        const clice: Asked = {
            server: await start("clice", opts.clice, workspace, {
                cdb,
                indexing: true,
                capabilities,
                project: { test_hooks: true, readonly: "auto" },
            }),
            expanded: new Map(),
        };
        clice.server.name = "clice-index";
        const cliceAst: Asked = {
            server: { ...clice.server, name: "clice-ast", millis: [], slowest: 0 },
            expanded: new Map(),
        };
        await clice.server.client.sync({ deadlineMs: 1_800_000 });
        let clangd: Asked | null = null;
        if (opts.clangd !== null) {
            clangd = {
                server: await start("clangd", opts.clangd, workspace, {
                    cdb,
                    indexing: true,
                    capabilities,
                    // The system headers of the compilers the database names,
                    // as clice takes them; every reference, as clice lists.
                    args: ["--query-driver=**", "--limit-references=0"],
                }),
                expanded: new Map(),
            };
            const first = paths[0];
            if (first !== undefined) {
                const [uri] = clangd.server.client.open(first);
                const indexStarted = clangd.server.client.progressed(
                    "backgroundIndexProgress",
                    "begin",
                );
                await clangd.server.client.hoverAt(uri, 0, 0);
                const began = await Promise.race([
                    indexStarted.then(() => true),
                    // No event: clangd announces no background index when its cache
                    // is current.
                    new Promise<boolean>((resolve) => {
                        setTimeout(() => {
                            resolve(false);
                        }, 20_000);
                    }),
                ]);
                if (began) {
                    const clangdStart = performance.now();
                    await clangd.server.client.progressed("backgroundIndexProgress", "end");
                    console.error(
                        `${files.name(workspace)}: clangd index ${((performance.now() - clangdStart) / 1000).toFixed(1)}s`,
                    );
                }
                clangd.server.client.close(uri);
            }
        }
        try {
            for (const file of paths) {
                await this.file(files, workspace, file, clice, cliceAst, clangd);
            }
            if (opts.symbols > 0) {
                await this.symbols(files, workspace, paths, clice, clangd);
            }
        } finally {
            for (const asked of [clice, cliceAst, clangd]) {
                if (asked === null || asked === cliceAst) {
                    continue;
                }
                this.report.timing.push(timing(asked.server));
            }
            this.report.timing.push(timing(cliceAst.server));
            await stop(clice.server);
            if (clangd !== null) {
                await stop(clangd.server);
            }
        }
    }

    private async file(
        files: Files,
        workspace: string,
        file: string,
        clice: Asked,
        cliceAst: Asked,
        clangd: Asked | null,
    ): Promise<void> {
        const opts = this.opts;
        const text = fs.readFileSync(file, "utf8").replace(/^\uFEFF/, "");
        const tokens = lex(text);
        const lines = new Lines(text);
        const name = files.name(file);
        const all = cursorTokens(text, tokens);
        const chosen =
            opts.at.length > 0
                ? opts.at
                      .filter((spec) => spec.startsWith(`${name}:`))
                      .map((spec) => {
                          const [line, column] = spec
                              .slice(name.length + 1)
                              .split(":")
                              .map(Number);
                          const offset = (lines.starts[(line ?? 1) - 1] ?? 0) + (column ?? 1) - 1;
                          return (
                              all.find((t) => t.begin <= offset && offset < t.end) ?? {
                                  kind: "ident" as const,
                                  begin: offset,
                                  end: offset + 1,
                                  directive: false,
                              }
                          );
                      })
                : sample(all, opts.maxPositions);
        if (chosen.length === 0) {
            return;
        }
        this.report.files += 1;
        const started = Date.now();
        const sites: Site[] = chosen.map((token) => ({
            file,
            offset: token.begin,
            token,
            spelling: text.slice(token.begin, token.end),
            lines,
            text,
        }));
        const neighbours = new Map<number, [string, string]>();
        for (const [i, token] of tokens.entries()) {
            const before = tokens[i - 1];
            const after = tokens[i + 1];
            neighbours.set(token.begin, [
                before === undefined ? "" : text.slice(before.begin, before.end),
                after === undefined ? "" : text.slice(after.begin, after.end),
            ]);
        }

        // Read-only serving ("auto") answers from the index until the first
        // edit intent, a completion request; the next request then pulls the
        // compile that makes the document's own AST answer.
        const [uri] = clice.server.client.open(file, 0, { text });
        await safely(clice.server.client.hoverAt(uri, 0, 0));
        const indexed = new Map<number, Answers>();
        await pool(sites, opts.jobs, async (site) => {
            indexed.set(site.offset, await ask(clice, files, uri, lines.position(site.offset)));
        });

        const compiledOpen = async (): Promise<string> => {
            const arrived = cliceAst.server.client.armDiagnostics(uri);
            await safely(cliceAst.server.client.completionAt(uri, 0, 0));
            await safely(cliceAst.server.client.hoverAt(uri, 0, 0));
            await withTimeout(arrived, 300_000, "clice-ast compile");
            return uri;
        };
        const servers: Asked[] = clangd === null ? [cliceAst] : [cliceAst, clangd];
        const opened = await Promise.allSettled(
            servers.map(async (asked) => {
                if (asked === cliceAst) {
                    return compiledOpen();
                }
                const [opened] = await withTimeout(
                    asked.server.client.openAndWait(file, 300_000, { text }),
                    300_000,
                    `${asked.server.name} open`,
                );
                return opened;
            }),
        );
        const uris: string[] = [];
        for (const [index, result] of opened.entries()) {
            if (result.status === "fulfilled") {
                uris.push(result.value);
            } else {
                this.report.failures.push(
                    `${servers[index]?.server.name ?? ""} ${name}: open failed: ${String(result.reason)}`,
                );
                uris.push("");
            }
        }
        servers.forEach((asked, index) => {
            const errors = (asked.server.client.diagnostics.get(uris[index] ?? "") ?? []).filter(
                (d) => d.severity === proto.DiagnosticSeverity.Error,
            );
            if (errors.length > 0) {
                const first = errors[0]?.message ?? "";
                const message = typeof first === "string" ? first : first.value;
                this.report.broken.push(
                    `${asked.server.name} ${name}: ${errors.length} errors, first: ${message}`,
                );
            }
        });
        const compiled = new Map<number, Answers>();
        const theirs = new Map<number, Answers>();
        await pool(sites, opts.jobs, async (site) => {
            const position = lines.position(site.offset);
            const [ours, other] = await Promise.all([
                uris[0] ? ask(cliceAst, files, uris[0], position) : Promise.resolve(null),
                clangd !== null && uris[1]
                    ? ask(clangd, files, uris[1], position)
                    : Promise.resolve(null),
            ]);
            if (ours !== null) {
                compiled.set(site.offset, ours);
            }
            if (other !== null) {
                theirs.set(site.offset, other);
            }
        });
        servers.forEach((asked, index) => {
            if (uris[index]) {
                asked.server.client.close(uris[index] ?? "");
            }
        });

        // `clice query` at the first positions the index answers.
        const cliAnswers = new Map<number, CliAnswers>();
        if (opts.cli > 0) {
            const answered = sites.filter((site) => {
                const answers = indexed.get(site.offset);
                return (answers?.references?.length ?? 0) > 0 && site.token.kind === "ident";
            });
            await pool(sample(answered, opts.cli), Math.max(1, opts.jobs >> 2), async (site) => {
                const position = lines.position(site.offset);
                const prefix = lines.lineText(position.line).slice(0, position.character);
                const column = Buffer.byteLength(prefix, "utf8") + 1;
                const locator = `${file}:${position.line + 1}:${column}`;
                cliAnswers.set(site.offset, await askCli(opts, files, workspace, locator));
            });
        }

        for (const site of sites) {
            this.report.positions += 1;
            const index = indexed.get(site.offset) ?? {};
            const ast = compiled.get(site.offset);
            const other = theirs.get(site.offset);
            const [previous, next] = neighbours.get(site.token.begin) ?? ["", ""];
            const cursor = cursorClass(site, previous, next);
            const symbol = this.symbolClass(
                files,
                site,
                [other, ast, index].filter((a): a is Answers => a !== undefined),
            );
            if (opts.at.length > 0) {
                const position = lines.position(site.offset);
                console.log(
                    `\n${name}:${position.line + 1}:${position.character + 1}  ${snippet(lines, { begin: site.token.begin, end: site.token.end })}  [${symbol}; ${cursor}]`,
                );
                for (const [label, answers] of [
                    ["clice-index", index],
                    ["clice-ast", ast],
                    ["clangd", other],
                ] as const) {
                    console.log(`  ${label}:`);
                    for (const request of REQUESTS) {
                        const value = answers?.[request];
                        if (value !== undefined && value !== null && value.length > 0) {
                            const shown = opts.full ? value.length : 6;
                            console.log(
                                `    ${request}: ${value
                                    .slice(0, shown)
                                    .map((k) => (request === "highlight" ? k : this.show(files, k)))
                                    .join(
                                        "\n        ",
                                    )}${value.length > shown ? ` (+${value.length - shown})` : ""}`,
                            );
                        }
                    }
                }
                const cli = cliAnswers.get(site.offset);
                if (cli !== undefined) {
                    console.log(`  cli: ${JSON.stringify(cli)}`);
                }
                continue;
            }
            if (ast !== undefined) {
                this.diff(
                    "index vs ast",
                    ["index", "ast"],
                    files,
                    site,
                    symbol,
                    cursor,
                    index,
                    ast,
                );
                if (other !== undefined) {
                    this.diff(
                        "clice vs clangd",
                        ["clice", "clangd"],
                        files,
                        site,
                        symbol,
                        cursor,
                        ast,
                        other,
                    );
                }
            }
            const cli = cliAnswers.get(site.offset);
            if (cli !== undefined) {
                this.cliDiff(files, site, symbol, index, cli);
            }
        }
        console.error(
            `${name}: ${sites.length} positions, ${((Date.now() - started) / 1000).toFixed(1)}s`,
        );
    }

    /// Search names of the files: clice, `clice query symbol-search` and
    /// clangd.
    private async symbols(
        files: Files,
        workspace: string,
        paths: string[],
        clice: Asked,
        clangd: Asked | null,
    ): Promise<void> {
        const opts = this.opts;
        const names = new Set<string>();
        for (const file of paths) {
            const text = fs.readFileSync(file, "utf8");
            for (const token of lex(text)) {
                const spelling = text.slice(token.begin, token.end);
                if (token.kind === "ident" && !KEYWORDS.has(spelling) && spelling.length >= 3) {
                    names.add(spelling);
                }
            }
        }
        const group = this.group("workspace symbol");
        const chosen = sample([...names].sort(), opts.symbols);
        interface Entry {
            /// The place and the bare name, as clangd names it.
            key: string;
            /// The place, the name and a function's signature, as the CLI
            /// lists them.
            full: string;
            name: string;
            kind: string;
            container: string;
        }
        const entries = (reply: proto.SymbolInformation[] | proto.WorkspaceSymbol[] | null) =>
            (reply ?? []).map((s): Entry => {
                const location = s.location as proto.Location;
                const start = "range" in location ? location.range.start.line + 1 : 0;
                const [qualified, signature] = splitSignature(s.name);
                const name = qualified.replace(/^.*::/, "");
                const place = `${files.name(URI.parse(location.uri).fsPath)}:${start}`;
                return {
                    key: `${place}:${name}`,
                    full: `${place}:${name}${signature}`,
                    name,
                    kind: SYMBOL_KINDS.get(s.kind) ?? String(s.kind),
                    container: s.containerName ?? "",
                };
            });
        await pool(chosen, opts.jobs, async (query) => {
            const [ours, other, cli] = await Promise.all([
                safely(clice.server.client.workspaceSymbols(query)),
                clangd === null
                    ? Promise.resolve(null)
                    : safely(clangd.server.client.workspaceSymbols(query)),
                cliQuery(opts, workspace, ["symbol-search", query, "--limit", "100"]),
            ]);
            const mine = entries(ours);
            const exact = (list: Entry[]) => list.filter((e) => e.name === query);
            const cliList = (
                (
                    cli.result as
                        | {
                              symbols?: {
                                  name: string;
                                  signature: string | null;
                                  file: string;
                                  line: number;
                              }[];
                          }
                        | undefined
                )?.symbols ?? []
            ).map((s) => `${files.name(s.file)}:${s.line}:${s.name}${s.signature ?? ""}`);
            const editorKeys = mine.map((e) => e.full);
            const sameCli = cliList.slice(0, 20).join("\n") === editorKeys.slice(0, 20).join("\n");
            this.agree("symbol cli top 20", sameCli);
            if (!sameCli) {
                const cliSet = new Set(cliList);
                const editorSet = new Set(editorKeys);
                const which =
                    cliList.length !== editorKeys.length
                        ? `count ${cliList.length} vs ${editorKeys.length}`
                        : editorKeys.every((k) => cliSet.has(k)) &&
                            cliList.every((k) => editorSet.has(k))
                          ? "same set, other order"
                          : "other symbols";
                group.add(`cli vs editor: ${which.replace(/\d+/g, "N")}`, query, () => ({
                    at: query,
                    detail: `${which}; cli ${cliList.slice(0, 2).join(" ")} editor ${editorKeys.slice(0, 2).join(" ")}`,
                }));
            }
            if (other === null || clangd === null) {
                return;
            }
            const theirs = entries(other);
            const myExact = exact(mine);
            const theirExact = exact(theirs);
            const myKeys = new Set(myExact.map((e) => e.key));
            const theirKeys = new Set(theirExact.map((e) => e.key));
            const same =
                myKeys.size === theirKeys.size && [...myKeys].every((k) => theirKeys.has(k));
            // A list cut at its limit holds an arbitrary share of the
            // matches beyond the ones both rank first.
            const cut = mine.length >= 100 || theirs.length >= 100;
            if (!cut) {
                this.agree("symbol exact matches vs clangd", same);
            }
            for (const e of cut ? [] : myExact.filter((e) => !theirKeys.has(e.key))) {
                group.add(`exact name: clice only ${e.kind}`, e.key, () => ({
                    at: query,
                    detail: this.show(files, e.key.replace(/:[^:]+$/, ":1")),
                }));
            }
            for (const e of cut ? [] : theirExact.filter((e) => !myKeys.has(e.key))) {
                group.add(`exact name: clangd only ${e.kind}`, e.key, () => ({
                    at: query,
                    detail: this.show(files, e.key.replace(/:[^:]+$/, ":1")),
                }));
            }
            for (const e of myExact) {
                const t = theirExact.find((x) => x.key === e.key);
                if (t === undefined) {
                    continue;
                }
                if (t.kind !== e.kind) {
                    group.add(`kind: clice ${e.kind} / clangd ${t.kind}`, e.key, () => ({
                        at: query,
                        detail: e.key,
                    }));
                }
                if (t.container !== e.container) {
                    group.add(`container differs`, e.key, () => ({
                        at: query,
                        detail: `${e.key} clice '${e.container}' clangd '${t.container}'`,
                    }));
                }
            }
            if (myExact.length === 0 && theirExact.length > 0) {
                group.add(`exact name: clice finds none`, query, () => ({
                    at: query,
                    detail: theirExact[0]?.key ?? "",
                }));
            }
            const first = mine[0];
            const theirFirst = theirs[0];
            if (first !== undefined && theirFirst !== undefined && first.name !== theirFirst.name) {
                group.add(`first result differs in name`, query, () => ({
                    at: query,
                    detail: `clice ${first.name} clangd ${theirFirst.name}`,
                }));
            }
        });
    }
}

function timing(server: Server): Record<string, unknown> {
    const sorted = [...server.millis].sort((a, b) => a - b);
    const at = (p: number) =>
        sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))] ?? 0;
    return {
        server: server.name,
        positions: sorted.length,
        p50_ms: at(0.5),
        p99_ms: at(0.99),
        slowest_ms: server.slowest,
    };
}

/// Each case under tests/<prefix> copied out, with its databases.
function caseWorkspaces(prefix: string): { root: string; files: string[] }[] {
    const base = path.join(CASES_DIR, prefix);
    const dirs: string[] = [];
    const walk = (dir: string) => {
        if (fs.existsSync(path.join(dir, "snap.toml"))) {
            dirs.push(dir);
            return;
        }
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            if (entry.isDirectory()) {
                walk(path.join(dir, entry.name));
            }
        }
    };
    walk(base);
    const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "navigation-check-"));
    return dirs.sort().map((dir) => {
        const rel = path.relative(CASES_DIR, dir);
        const root = path.join(scratch, rel);
        fs.mkdirSync(root, { recursive: true });
        const copied = materialize(dir, root);
        const chain: string[] = [];
        for (let d = dir; d.startsWith(CASES_DIR) && d !== CASES_DIR; d = path.dirname(d)) {
            chain.unshift(d);
        }
        const project = mergeProjects(chain.map((d) => readProject(d)));
        writeDatabases(project, copied, root);
        return {
            root,
            files: copied.filter((f) => MARKED.test(f)).map((f) => path.join(root, f)),
        };
    });
}

async function main(): Promise<void> {
    const opts = options();
    const checker = new Checker(opts);
    if (opts.cases !== null) {
        for (const { root, files } of caseWorkspaces(opts.cases)) {
            try {
                await checker.workspace(root, root, files);
            } catch (error) {
                checker.report.failures.push(`${root}: ${String(error).slice(0, 300)}`);
            }
        }
    } else {
        await checker.workspace(opts.workspace, opts.cdb, opts.files);
    }
    if (opts.at.length > 0) {
        return;
    }
    const report = checker.report;
    if (opts.json !== null) {
        fs.writeFileSync(
            opts.json,
            JSON.stringify(
                {
                    ...report,
                    groups: Object.fromEntries(
                        Object.entries(report.groups).map(([name, groups]) => [
                            name,
                            Object.fromEntries(groups.sorted()),
                        ]),
                    ),
                },
                null,
                2,
            ),
        );
    }
    console.log(`files ${report.files}, positions ${report.positions}`);
    console.log(JSON.stringify(report.timing));
    for (const [what, { compared, agreed }] of Object.entries(report.agreements).sort()) {
        console.log(`${what.padEnd(40)} ${agreed}/${compared}`);
    }
    for (const failure of report.failures.slice(0, 20)) {
        console.log(`FAILURE ${failure}`);
    }
    console.log(`${report.broken.length} file opens with compile errors`);
    for (const line of report.broken.slice(0, 20)) {
        console.log(`BROKEN ${line}`);
    }
    for (const [title, groups] of Object.entries(report.groups)) {
        console.log(`\n== ${title}`);
        for (const [key, group] of groups.sorted().slice(0, 80)) {
            console.log(`${String(group.count).padStart(6)}  ${key}`);
            for (const example of group.examples.slice(0, 2)) {
                console.log(`          ${example.at}  ${example.detail}`);
            }
        }
    }
}

await main();
