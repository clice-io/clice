/// The features on real files: each sampled file asked the same requests,
/// at the same places, of `clice inspect` and of a server over the indexed
/// project, the two answers compared; a file inspect cannot compile alone
/// (a module importer) is asked of the server only. The server is also
/// asked to navigate from every place, which only it answers. A request
/// that fails, hangs or crashes a worker is a failure, as is a place
/// where the two answers differ.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { createTwoFilesPatch } from "diff";
import * as proto from "vscode-languageserver-protocol";
import { KEYWORDS, lex } from "../bench/lsp_check.ts";
import { Workspace } from "../client/workspace.ts";
import { withTimeout } from "../promise.ts";
import type { MarkerAnchor } from "../testing/cases/anchors.ts";
import { Disk } from "../testing/cases/disk.ts";
import { importsModule, Inspect } from "../testing/inspect/inspect.ts";
import type { Diagnostic } from "../testing/render/answers.ts";
import type { Client } from "../testing/render/client.ts";
import { withoutBom } from "../testing/render/position.ts";
import { block, type Shown } from "../testing/render/shown.ts";
import { writeToml } from "../testing/render/value.ts";
import { Serve } from "../testing/server/serve.ts";
import { StepRecord } from "../testing/server/steps.ts";

/// Places per file: points for the requests about a symbol, a few member
/// accesses for completion and calls for signature help, each of which
/// inspect compiles once.
const POINTS = 20;
const COMPLETIONS = 3;

/// Files asked at once: inspect compiles each alone beside the server's
/// compile of it, a large unit taking 3 GB in each, so one file per 5 GB of
/// the machine's memory, at most three.
const CONCURRENCY = Math.min(3, Math.max(1, Math.floor(os.totalmem() / 5 / 1024 ** 3)));

/// A request that answers in no time on a test case may compile a large
/// unit here: the bound only a hang reaches.
const REQUEST_MS = 10 * 60_000;

/// A file's ~150 requests can each take most of their bound: the files in
/// flight at the deadline are asked this much longer, the rest left unasked.
const FINISH_MS = 30 * 60_000;

/// A request slower than this is recorded with its time.
const SLOW_MS = 60_000;

/// The client an editor is: what the answers render for in the cases too.
const CAPABILITIES: proto.ClientCapabilities = {
    workspace: { workspaceEdit: { documentChanges: true } },
    textDocument: {
        hover: { contentFormat: ["markdown", "plaintext"] },
        completion: {
            completionItem: {
                snippetSupport: true,
                insertReplaceSupport: true,
                documentationFormat: ["markdown", "plaintext"],
            },
        },
        inlayHint: {},
        rename: { prepareSupport: true },
        publishDiagnostics: { relatedInformation: true },
        documentSymbol: {
            hierarchicalDocumentSymbolSupport: true,
            symbolKind: { valueSet: Object.values(proto.SymbolKind) },
        },
    },
};

export const REQUESTS = [
    "semanticTokens",
    "foldingRanges",
    "documentSymbols",
    "documentLinks",
    "inlayHints",
    "hover",
    "highlights",
    "selectionRange",
    "completion",
    "signatureHelp",
] as const;

/// The requests only the server answers: navigation over the index.
export const NAVIGATION = ["definition", "references", "callHierarchy", "typeHierarchy"] as const;

export type Request = (typeof REQUESTS)[number] | (typeof NAVIGATION)[number] | "errors";

const SOURCE = /\.(c|cc|cpp|cxx|c\+\+|cppm|ixx|cu)$/;

/// The differences between inspect and the server the project accepts, each
/// pinned by a case: two answers equal once `same` rewrites both show one.
const ACCEPTED: { case: string; request: Request; same: (shown: string) => string }[] = [
    {
        // Under a preamble a default argument written as a macro can print
        // as its value: a clang SourceManager bug.
        case: "completion/member_access/20_macro_default_arguments",
        request: "completion",
        same: (shown) => shown.replaceAll(/ = [^,()"]+(?=[,)])/g, " = ..."),
    },
];

/// The case pinning the difference between two rendered answers, when the
/// project accepts it.
export function acceptedDifference(
    request: Request,
    inspect: string,
    server: string,
): string | undefined {
    return ACCEPTED.find(
        (accepted) =>
            accepted.request === request && accepted.same(inspect) === accepted.same(server),
    )?.case;
}

/// The name a module declaration declares: the server answers about it from
/// the index of module interfaces, which inspect does not build.
const MODULE_NAME = /(?<=^[ \t]*(?:export[ \t]+)?module[ \t]+)[\w.:]+/gm;

/// A deterministic shuffle: the same seed picks the same files.
function shuffled<T>(items: readonly T[], seed: string): T[] {
    let state = 2166136261;
    for (let i = 0; i < seed.length; i += 1) {
        state = Math.imul(state ^ seed.charCodeAt(i), 16777619) >>> 0;
    }
    const random = () => {
        state = (state + 0x6d2b79f5) >>> 0;
        let t = state;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
    const out = [...items];
    for (let i = out.length - 1; i > 0; i -= 1) {
        const j = Math.floor(random() * (i + 1));
        [out[i], out[j]] = [out[j] as T, out[i] as T];
    }
    return out;
}

/// `count` of the units' source files, the seed choosing which.
export function sampleFiles(units: readonly string[], count: number, seed: string): string[] {
    const files = [...new Set(units.filter((unit) => SOURCE.test(unit)))].sort();
    return shuffled(files, seed).slice(0, count).sort();
}

function spread<T>(items: readonly T[], count: number): T[] {
    if (items.length <= count) {
        return [...items];
    }
    return Array.from(
        { length: count },
        (_, i) => items[Math.floor(((i + 0.5) * items.length) / count)] as T,
    );
}

/// The places a file is asked at, the same every night: identifiers
/// spread over the file, member accesses, and the insides of calls.
export function placesOf(
    file: string,
    text: string,
): Record<"points" | "members" | "calls", MarkerAnchor[]> {
    const tokens = lex(text);
    const word = (index: number): string => {
        const token = tokens[index];
        return token === undefined ? "" : text.slice(token.begin, token.end);
    };
    const place = (name: string, at: number): MarkerAnchor => ({
        path: file,
        name,
        begin: at,
        end: at,
        range: false,
        text,
    });
    const moduleNames = [...text.matchAll(MODULE_NAME)].map((match) => ({
        begin: match.index,
        end: match.index + match[0].length,
    }));
    const identifiers: number[] = [];
    const members: number[] = [];
    const calls: number[] = [];
    tokens.forEach((token, index) => {
        if (
            token.directive ||
            token.kind !== "ident" ||
            KEYWORDS.has(word(index)) ||
            moduleNames.some((name) => token.begin >= name.begin && token.end <= name.end)
        ) {
            return;
        }
        identifiers.push(token.begin + Math.floor((token.end - token.begin) / 2));
        if ([".", "->", "::"].includes(word(index - 1))) {
            members.push(token.begin);
        }
        const next = tokens[index + 1];
        if (word(index + 1) === "(" && next !== undefined && !next.directive) {
            calls.push(next.end);
        }
    });
    return {
        points: spread(identifiers, POINTS).map((at, i) => place(`p${String(i)}`, at)),
        members: spread(members, COMPLETIONS).map((at, i) => place(`m${String(i)}`, at)),
        calls: spread(calls, COMPLETIONS).map((at, i) => place(`c${String(i)}`, at)),
    };
}

interface Ask {
    request: Request;
    label: string;
    ask: (client: Client) => Shown;
}

function asksOf(
    file: string,
    places: ReturnType<typeof placesOf>,
    only: ReadonlySet<Request> | undefined,
): { compared: Ask[]; navigation: Ask[] } {
    const asks: Ask[] = [
        { request: "semanticTokens", label: "semanticTokens", ask: (c) => c.semanticTokens(file) },
        { request: "foldingRanges", label: "foldingRanges", ask: (c) => c.foldingRanges(file) },
        {
            request: "documentSymbols",
            label: "documentSymbols",
            ask: (c) => c.documentSymbols(file),
        },
        { request: "documentLinks", label: "documentLinks", ask: (c) => c.documentLinks(file) },
        { request: "inlayHints", label: "inlayHints", ask: (c) => c.inlayHints(file) },
    ];
    const navigation: Ask[] = [];
    for (const point of places.points) {
        asks.push(
            { request: "hover", label: `hover ${point.name}`, ask: (c) => c.hover(point) },
            {
                request: "highlights",
                label: `highlights ${point.name}`,
                ask: (c) => c.highlights(point),
            },
            {
                request: "selectionRange",
                label: `selectionRange ${point.name}`,
                ask: (c) => c.selectionRange(point),
            },
        );
        navigation.push(
            {
                request: "definition",
                label: `definition ${point.name}`,
                ask: (c) => c.definition(point),
            },
            {
                request: "references",
                label: `references ${point.name}`,
                ask: (c) => c.references(point),
            },
            {
                request: "callHierarchy",
                label: `callHierarchy ${point.name}`,
                ask: (c) => c.callHierarchy(point),
            },
            {
                request: "typeHierarchy",
                label: `typeHierarchy ${point.name}`,
                ask: (c) => c.typeHierarchy(point),
            },
        );
    }
    for (const member of places.members) {
        asks.push({
            request: "completion",
            label: `completion ${member.name}`,
            ask: (c) => c.completion(member),
        });
    }
    for (const call of places.calls) {
        asks.push({
            request: "signatureHelp",
            label: `signatureHelp ${call.name}`,
            ask: (c) => c.signatureHelp(call),
        });
    }
    const wanted = (ask: Ask) => only === undefined || only.has(ask.request);
    return { compared: asks.filter(wanted), navigation: navigation.filter(wanted) };
}

/// What a client answered to one request: the answer itself, or why there
/// was none.
interface Answer {
    reply?: Shown;
    raw?: unknown;
    value?: string;
    error?: string;
    /// A request that never answered, or an inspect that died.
    stopped?: "hang" | "crash";
    ms: number;
}

/// What of an answer the sweep keeps until the file is done: what is
/// compared and shown on a mismatch, the value alone (the errors a file
/// opens with), or nothing (navigation, which can list thousands of places
/// over a large index).
type Keep = "compared" | "value" | "nothing";

async function answer(client: Client, ask: Ask, keep: Keep): Promise<Answer> {
    const started = Date.now();
    const reply = ask.ask(client);
    try {
        const value: unknown = await withTimeout(reply as Promise<unknown>, REQUEST_MS, ask.label);
        const ms = Date.now() - started;
        return keep === "compared"
            ? { reply, value: JSON.stringify(value), ms }
            : keep === "value"
              ? { raw: value, ms }
              : { ms };
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        const stopped = message.includes("timed out after")
            ? "hang"
            : message.startsWith("clice inspect exited SIG")
              ? "crash"
              : undefined;
        return {
            error: message.slice(0, 2000),
            ...(stopped === undefined ? {} : { stopped }),
            ms: Date.now() - started,
        };
    }
}

/// The text an answer shows, in full and as a snapshot holds it: what the
/// two clients must agree on.
async function rendered(reply: Shown, root: string, label: string): Promise<string> {
    try {
        return writeToml([
            {
                path: [label],
                table: await block(reply, { parts: new Set(), full: true }, root, label),
            },
        ]);
    } catch (error) {
        return `cannot render: ${error instanceof Error ? error.message : String(error)}`;
    }
}

export interface SweepFailure {
    kind: "disagree" | "inspect" | "server" | "hang" | "crash";
    file: string;
    request: Request;
    label: string;
    detail: string;
}

export interface SweepRun {
    files: string[];
    /// The files asked of the server only, each with the first error inspect
    /// compiled it with.
    serverOnly: { file: string; why: string }[];
    requests: number;
    inspectSeconds: number;
    serverSeconds: number;
    failures: SweepFailure[];
    /// The places whose answers differ as the project accepts, each with
    /// the case pinning it.
    accepted: { file: string; label: string; case: string }[];
    /// Why the server failed its clean exit: a crash, or no exit at all.
    serverExit: string[];
    /// The files the server opened with errors, the first few of each.
    serverErrors: { file: string; errors: string[] }[];
    /// The files left unasked when the sweep ran out of time, and those it
    /// ran out of time in.
    unasked: string[];
    cut: string[];
    /// The requests slower than SLOW_MS.
    slow: { file: string; label: string; side: "inspect" | "server"; seconds: number }[];
}

export interface SweepOptions {
    root: string;
    files: readonly string[];
    /// No file is begun after this time.
    deadline: number;
    only?: ReadonlySet<Request>;
}

async function sweepFile(
    options: SweepOptions,
    server: Serve,
    file: string,
    places: ReturnType<typeof placesOf>,
    run: SweepRun,
): Promise<void> {
    const text = withoutBom(fs.readFileSync(path.join(options.root, file), "utf8"));
    const { compared, navigation } = asksOf(file, places, options.only);
    const fail = (kind: SweepFailure["kind"], ask: Ask, detail: string) =>
        run.failures.push({ kind, file, request: ask.request, label: ask.label, detail });
    const cutoff = options.deadline + FINISH_MS;
    const cut = new Set<"inspect" | "server">();

    const inspected = (async () => {
        const answers = new Map<string, Answer>();
        if (importsModule(text)) {
            return { answers, errors: ["imports a module"] };
        }
        if (compared.length === 0) {
            return { answers, errors: [] };
        }
        const started = Date.now();
        // Inspect asks a request at every place of its client in one run, a
        // compile per place for completion and signature help: each gets a
        // client of its own places only.
        const inspect = (marks: MarkerAnchor[]) =>
            new Inspect(options.root, marks, { sources: [file] });
        const atPoints = inspect(places.points);
        const atMembers = inspect(places.members);
        const atCalls = inspect(places.calls);
        const clientOf = (ask: Ask) =>
            ask.request === "completion"
                ? atMembers
                : ask.request === "signatureHelp"
                  ? atCalls
                  : atPoints;
        let errors: string[];
        try {
            for (const ask of compared) {
                // A run that crashed or hung once on the file does again at
                // its other places, and each costs up to its bound.
                if ([...answers.values()].some((each) => each.stopped !== undefined)) {
                    break;
                }
                if (Date.now() > cutoff) {
                    cut.add("inspect");
                    break;
                }
                answers.set(ask.label, await answer(clientOf(ask), ask, "compared"));
            }
            // A file importing a module through a header compiles with
            // errors alone, which every answer of it shows.
            errors = await atPoints.compileErrors().catch((error: unknown) => [String(error)]);
        } finally {
            for (const client of [atPoints, atMembers, atCalls]) {
                client.close();
            }
            run.inspectSeconds += (Date.now() - started) / 1000;
        }
        return { answers, errors };
    })();
    // The file opens as in an editor, its first compile done before the
    // requests.
    const opened: Ask = {
        request: "errors",
        label: "errors",
        ask: () => server.errors(file),
    };
    const served = (async () => {
        const answers = new Map<string, Answer>();
        const started = Date.now();
        const child = server.client.child;
        const alive = () => child.exitCode === null && child.signalCode === null;
        for (const ask of [opened, ...compared, ...navigation]) {
            // A hung request holds the document in its worker, and a dead
            // server answers nothing: the file's other requests would only
            // wait their bound out, or fail for the same reason.
            if (!alive() || [...answers.values()].some((each) => each.stopped === "hang")) {
                break;
            }
            if (Date.now() > cutoff) {
                cut.add("server");
                break;
            }
            const keep = ask === opened ? "value" : compared.includes(ask) ? "compared" : "nothing";
            answers.set(ask.label, await answer(server, ask, keep));
        }
        try {
            server.close(file);
        } catch (error) {
            // The server's exit reports its death.
            if (alive()) {
                throw error;
            }
        }
        run.serverSeconds += (Date.now() - started) / 1000;
        return answers;
    })();
    const [{ answers: fromInspect, errors }, fromServer] = await Promise.all([inspected, served]);
    if (cut.size > 0) {
        run.cut.push(file);
    }
    for (const [side, answers] of [
        ["inspect", fromInspect],
        ["server", fromServer],
    ] as const) {
        for (const [label, { ms }] of answers) {
            if (ms > SLOW_MS) {
                run.slow.push({ file, label, side, seconds: Math.round(ms / 1000) });
            }
        }
    }
    const [why] = errors;
    if (why !== undefined) {
        run.serverOnly.push({ file, why });
    }
    // A project's clang-tidy configuration may raise its findings to errors;
    // only the compile's own are the compiler's to judge.
    const opening = ((fromServer.get(opened.label)?.raw ?? []) as Diagnostic[]).filter(
        (diagnostic) => diagnostic.source !== "clang-tidy",
    );
    if (opening.length > 0) {
        run.serverErrors.push({
            file,
            errors: opening
                .slice(0, 5)
                .map(
                    ({ location, message }) =>
                        `${location.path}:${String(location.range.start.line + 1)}:${String(location.range.start.character + 1)}: ${message}`,
                ),
        });
    }

    run.requests += fromServer.size + fromInspect.size;
    for (const ask of [opened, ...compared, ...navigation]) {
        const right = fromServer.get(ask.label) ?? { ms: 0 };
        if (right.error !== undefined) {
            fail(right.stopped ?? "server", ask, right.error);
        }
        if (!compared.includes(ask)) {
            continue;
        }
        const left = fromInspect.get(ask.label) ?? { ms: 0 };
        if (left.stopped !== undefined) {
            fail(left.stopped, ask, left.error ?? "");
            continue;
        }
        if (why !== undefined) {
            continue;
        }
        if (left.error !== undefined) {
            fail("inspect", ask, left.error);
            continue;
        }
        if (
            right.error !== undefined ||
            left.value === right.value ||
            left.reply === undefined ||
            right.reply === undefined
        ) {
            continue;
        }
        const [shownLeft, shownRight] = await Promise.all([
            rendered(left.reply, options.root, ask.label),
            rendered(right.reply, options.root, ask.label),
        ]);
        if (shownLeft === shownRight) {
            continue;
        }
        const accepted = acceptedDifference(ask.request, shownLeft, shownRight);
        if (accepted !== undefined) {
            run.accepted.push({ file, label: ask.label, case: accepted });
            continue;
        }
        fail(
            "disagree",
            ask,
            createTwoFilesPatch("inspect", "server", shownLeft, shownRight, "", "", {
                context: 3,
            }),
        );
    }
}

export async function sweep(options: SweepOptions): Promise<SweepRun> {
    const run: SweepRun = {
        files: [...options.files],
        serverOnly: [],
        requests: 0,
        inspectSeconds: 0,
        serverSeconds: 0,
        failures: [],
        accepted: [],
        serverExit: [],
        serverErrors: [],
        unasked: [],
        cut: [],
        slow: [],
    };
    const places = options.files.map(
        (file) =>
            [
                file,
                placesOf(file, withoutBom(fs.readFileSync(path.join(options.root, file), "utf8"))),
            ] as const,
    );
    const marks = places.flatMap(([, each]) => [...each.points, ...each.members, ...each.calls]);
    const steps = new StepRecord();
    const server = new Serve(
        new Workspace(options.root),
        { capabilities: CAPABILITIES, config: { project: { enable_indexing: false } } },
        { marks, steps, disk: new Disk(options.root, {}, steps) },
    );
    try {
        await server.start();
        const queue = [...places];
        const worker = async () => {
            for (let next = queue.shift(); next !== undefined; next = queue.shift()) {
                if (Date.now() > options.deadline) {
                    run.unasked.push(next[0]);
                    continue;
                }
                const [file, filePlaces] = next;
                const heap = Math.round(process.memoryUsage().heapUsed / 1024 / 1024);
                process.stderr.write(`sweep ${file} (runner heap ${String(heap)} MB)\n`);
                await sweepFile(options, server, file, filePlaces, run);
            }
        };
        await Promise.all(Array.from({ length: CONCURRENCY }, worker));
    } finally {
        run.serverExit = (await server.finish(false)).map((error) =>
            error instanceof Error ? error.message : String(error),
        );
    }
    return run;
}
