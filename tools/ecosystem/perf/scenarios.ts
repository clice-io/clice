/// The perf scenarios a project's test.ts runs (ecosystem/README.md, Perf):
/// each over every server, alternating them run by run on the same files,
/// so that a drift of the machine reaches all alike.

import * as fs from "node:fs";
import * as path from "node:path";
import { pathToFileURL } from "node:url";
import * as proto from "vscode-languageserver-protocol";
import { runProcess } from "../../client/process.ts";
import { entrySource, type DatabaseEntry } from "../../compat/database.ts";
import {
    includedHeaders,
    inside,
    sampleFiles,
    sampleHeaders,
    scanFiles,
    shuffled,
} from "../database.ts";
import {
    identifierAt,
    positionOf,
    PROBES,
    type PerfFile,
    type Place,
    type Probe,
} from "../script.ts";
import type { RunResult, ServerName } from "./results.ts";
import { cacheMB, dropFileCache, isClice, wipeCache } from "./servers.ts";
import { bounded, median, nowMs, pause, Session, sizeOf, type PerfContext } from "./session.ts";

/// A file a scenario opens: its path alone, or with its request positions.
export type FileArg = string | PerfFile;

/// A file's request positions, resolved.
interface Opened {
    file: string;
    at: Partial<Record<Probe, proto.Position>>;
    query?: string;
}

/// Hang detection: the bound on one settling of the servers after a churn,
/// which on LLVM reindexes for over half an hour.
const SETTLE_MS = 30 * 60_000;

/// Document symbols counted through their children.
function symbolCount(answer: unknown): number {
    if (!Array.isArray(answer)) {
        return 0;
    }
    return (answer as { children?: unknown }[]).reduce(
        (sum: number, symbol) => sum + 1 + symbolCount(symbol.children),
        0,
    );
}

/// The distinct files references reach.
function filesOf(answer: unknown): number {
    return Array.isArray(answer) ? new Set(answer.map((at: { uri: string }) => at.uri)).size : 0;
}

function round(value: number): number {
    return Math.round(value * 10) / 10;
}

export class Perf {
    private readonly context: PerfContext;
    private readonly entries: readonly DatabaseEntry[];
    private readonly runs: RunResult[];
    private readonly only: ReadonlySet<string> | undefined;
    private readonly save: () => void;

    constructor(
        context: PerfContext,
        entries: readonly DatabaseEntry[],
        runs: RunResult[],
        options: { only?: ReadonlySet<string>; save: () => void },
    ) {
        this.context = context;
        this.entries = entries;
        this.runs = runs;
        this.only = options.only;
        this.save = options.save;
    }

    /// The checkout.
    get root(): string {
        return this.context.workspace;
    }

    private servers(): ServerName[] {
        return (["clice", "clangd", "baseline"] as const).filter(
            (server) => this.context.binaries[server] !== undefined,
        );
    }

    private resolve(arg: FileArg): Opened {
        const file = typeof arg === "string" ? { file: arg } : arg;
        const at: Partial<Record<Probe, proto.Position>> = {};
        for (const probe of PROBES) {
            const place: Place | undefined = file[probe];
            if (place !== undefined) {
                at[probe] = positionOf(this.root, file.file, place);
            }
        }
        const query =
            file.query ??
            (at.symbol === undefined ? undefined : identifierAt(this.root, file.file, at.symbol));
        return { file: file.file, at, ...(query === undefined ? {} : { query }) };
    }

    /// Every run of a scenario: `runs` rounds, the servers alternating in
    /// each, every run a fresh server. A run that fails is a result too:
    /// the job goes on to the others.
    private async each(
        scenario: string,
        label: string,
        options: {
            runs?: number;
            servers?: readonly ServerName[];
            /// The server's cache, wiped before each run; for clice its PCHs
            /// alone, as `variant` says.
            wipe?: "cache" | "pch";
            variant?: string;
            file?: string;
        },
        body: (session: Session) => Promise<void>,
    ): Promise<void> {
        if (this.only !== undefined && !this.only.has(scenario)) {
            return;
        }
        for (let run = 1; run <= (options.runs ?? 1); run += 1) {
            for (const server of options.servers ?? this.servers()) {
                const result: RunResult = {
                    server,
                    ...(options.variant === undefined ? {} : { variant: options.variant }),
                    scenario,
                    label,
                    ...(options.file === undefined ? {} : { file: options.file }),
                    run,
                    series: {},
                    sizes: {},
                    values: {},
                    snapshots: {},
                };
                const session = new Session(this.context, result);
                try {
                    if (options.wipe !== undefined) {
                        wipeCache(
                            this.context.workspace,
                            this.context.database,
                            server,
                            options.wipe === "pch" ? "pch" : undefined,
                        );
                        if (this.context.dropCaches) {
                            result.dropped = dropFileCache();
                        }
                    }
                    await body(session);
                } catch (error) {
                    result.error = error instanceof Error ? error.message : String(error);
                }
                try {
                    await session.finish();
                } catch (error) {
                    result.error ??= error instanceof Error ? error.message : String(error);
                }
                this.runs.push(result);
                console.log(
                    `[${scenario} ${options.file ?? label} #${String(run)} ${server}${options.variant === undefined ? "" : ` ${options.variant}`}] ${result.error ?? "done"}`,
                );
                this.save();
            }
        }
    }

    /// Fixed-seed files for the scenarios over many: `sources` of the
    /// database's units and `headers` they include, under the directories
    /// `under` names (the whole checkout by default).
    async pick(options: {
        seed?: string;
        sources: number;
        headers?: number;
        under?: readonly string[];
    }): Promise<string[]> {
        const seed = options.seed ?? "1";
        const under = (file: string) =>
            options.under === undefined || options.under.some((dir) => file.startsWith(`${dir}/`));
        const units = this.entries
            .flatMap((entry) => inside(this.root, entrySource(entry)) ?? [])
            .filter(under);
        const clice = this.context.binaries.clice ?? "";
        const headers =
            (options.headers ?? 0) === 0
                ? []
                : [...(await includedHeaders(this.root, clice, false)).keys()].filter(under);
        return [
            ...sampleFiles(units, options.sources, seed),
            ...sampleHeaders(headers, options.headers ?? 0, seed),
        ];
    }

    /// Server start to the opened file's first diagnostics, and to the first
    /// non-empty semantic tokens, hover and definition, asked right at the
    /// open as an editor does; a warm start also times the first references
    /// reaching a second file — the saved index loaded, as clangd's index of
    /// the open file holds no other file's references.
    private async start(session: Session, opened: Opened, warm: boolean): Promise<void> {
        const client = await session.start();
        const begun = nowMs();
        const [uri] = client.open(path.resolve(this.root, opened.file));
        let built = false;
        let indexed = false;
        const afterBuild = (): boolean => built;
        const waits = [
            bounded(client.armDiagnostics(uri), `diagnostics of ${opened.file}`).then(() => {
                session.record("open_to_diagnostics", nowMs() - begun);
                built = true;
            }),
            session.untilAnswered(
                "open_to_semantic_tokens",
                begun,
                () => client.semanticTokensFull(uri),
                afterBuild,
            ),
        ];
        const symbol = opened.at.symbol;
        if (symbol !== undefined) {
            waits.push(
                session.untilAnswered(
                    "open_to_hover",
                    begun,
                    () => client.hoverAt(uri, symbol.line, symbol.character),
                    afterBuild,
                ),
                session.untilAnswered(
                    "open_to_definition",
                    begun,
                    () => client.definitionAt(uri, symbol.line, symbol.character),
                    afterBuild,
                ),
            );
        }
        if (warm && symbol !== undefined) {
            const afterLoad = (): boolean => built && indexed;
            waits.push(
                session.idle(false).then(() => {
                    indexed = true;
                }),
                session.untilAnswered(
                    "open_to_cross_file_references",
                    begun,
                    () => client.referencesAt(uri, symbol.line, symbol.character),
                    afterLoad,
                    (answer) => (filesOf(answer) >= 2 ? sizeOf(answer) : 0),
                ),
            );
        }
        await Promise.all(waits);
    }

    /// No cache: start to the file's first answers.
    async coldStart(file: FileArg, options: { runs?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        await this.each("coldStart", opened.file, { runs: options.runs ?? 5, wipe: "cache" }, (s) =>
            this.start(s, opened, false),
        );
    }

    /// On the index an earlier scenario left: start to the file's first
    /// answers, references across files included.
    async warmStart(file: FileArg, options: { runs?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        await this.each("warmStart", opened.file, { runs: options.runs ?? 5 }, (s) =>
            this.start(s, opened, true),
        );
    }

    /// No cache, the file open as the editor that started the server has it
    /// (clangd finds its database through an opened file): until the
    /// background index of the whole project is idle; what the index and
    /// the whole cache hold on disk once the server is down, and with
    /// `hold` what the tree holds that many seconds later.
    async index(file: FileArg, options: { hold?: number } = {}): Promise<void> {
        await this.indexRun("index", file, { wrap: [], ...options });
    }

    /// (Linux) `index` with the server's tree inside a memory limit of `mb`
    /// megabytes, past which the kernel kills its processes; an index not
    /// done within `minutes` (30 by default) has stalled.
    async limitedIndex(file: FileArg, options: { mb: number; minutes?: number }): Promise<void> {
        if (process.platform !== "linux") {
            throw new Error("limitedIndex runs on Linux alone, the one system with memory scopes");
        }
        const wrap = [
            "sudo",
            "-n",
            "-E",
            `PATH=${process.env["PATH"] ?? ""}`,
            "systemd-run",
            "--scope",
            "--quiet",
            `--uid=${String(process.getuid?.() ?? 0)}`,
            `--gid=${String(process.getgid?.() ?? 0)}`,
            "-p",
            `MemoryMax=${String(options.mb)}M`,
            "-p",
            "MemorySwapMax=0",
            "-p",
            "OOMPolicy=continue",
            "--",
        ];
        await this.indexRun("limitedIndex", file, {
            wrap,
            limit: `${String(options.mb)} MB`,
            minutes: options.minutes ?? 30,
        });
    }

    private async indexRun(
        scenario: string,
        file: FileArg,
        options: { wrap: readonly string[]; hold?: number; limit?: string; minutes?: number },
    ): Promise<void> {
        const opened = this.resolve(file);
        const { workspace, database } = this.context;
        const label =
            options.limit === undefined ? opened.file : `${opened.file}, ${options.limit}`;
        const bound = options.minutes === undefined ? undefined : options.minutes * 60_000;
        await this.each(scenario, label, { wipe: "cache" }, async (s) => {
            await s.start(options.wrap);
            await s.open(opened.file);
            const { ended, files, idle } = await s.idle(true, bound);
            if (!idle) {
                throw new Error(
                    `${s.server}: the background index is not idle after ${String(Math.round((nowMs() - s.started) / 60_000))} minutes`,
                );
            }
            s.record("index", ended - s.started);
            s.result.values["indexed files"] = files;
            s.snapshot("indexed");
            if (options.hold !== undefined) {
                // Time is the feature: what an idle server gives back.
                await pause(options.hold * 1000);
                s.snapshot("held");
            }
            // Sized once the server is down: clice saves its index after the
            // round ends, and again at exit.
            await s.finish();
            const { diskMB, indexMB } = cacheMB(workspace, database, s.server);
            s.result.values["index MB on disk"] = indexMB;
            s.result.values["cache MB on disk"] = diskMB;
        });
    }

    /// Edits at the file's end, each followed by semantic tokens: the times
    /// to the new diagnostics and tokens.
    async editLoop(file: FileArg, options: { edits?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        await this.each("editLoop", opened.file, {}, async (s) => {
            await s.start();
            const { uri } = await s.open(opened.file);
            for (let i = 0; i < (options.edits ?? 20); i += 1) {
                const { diagnostics, tokens } = await s.edit(uri);
                s.record("edit_to_diagnostics", diagnostics);
                s.record("edit_to_semantic_tokens", tokens);
            }
        });
    }

    /// The index loaded, as in a session that has run a while: each request
    /// once unmeasured, then `repeats` times.
    async warmRequests(file: FileArg, options: { repeats?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        await this.each("warmRequests", opened.file, {}, async (s) => {
            const client = await s.start();
            const { uri } = await s.open(opened.file);
            await s.idle(false);
            const requests: Record<string, () => Promise<unknown>> = {};
            const symbol = opened.at.symbol;
            if (symbol !== undefined) {
                const { line, character } = symbol;
                requests["hover"] = () => client.hoverAt(uri, line, character);
                requests["definition"] = () => client.definitionAt(uri, line, character);
                requests["references"] = () => client.referencesAt(uri, line, character);
                requests["call_hierarchy"] = async () => {
                    const items = await client.prepareCallHierarchy(uri, line, character);
                    const item = items?.[0];
                    return item === undefined ? [] : client.callHierarchyIncoming(item);
                };
            }
            const query = opened.query;
            if (query !== undefined) {
                requests["workspace_symbol"] = () => client.workspaceSymbols(query);
            }
            requests["document_symbol"] = () => client.documentSymbols(uri);
            requests["semantic_tokens"] = () => client.semanticTokensFull(uri);
            requests["folding_range"] = () => client.foldingRanges(uri);
            const shownLine = symbol?.line ?? 0;
            requests["inlay_hint"] = () =>
                client.inlayHints(uri, {
                    start: { line: Math.max(0, shownLine - 50), character: 0 },
                    end: { line: shownLine + 50, character: 0 },
                });
            for (const [name, probe] of [
                ["completion_member", "member"],
                ["completion_unqualified", "unqualified"],
                ["completion_template", "template"],
            ] as const) {
                const at = opened.at[probe];
                if (at !== undefined) {
                    requests[name] = () => client.completionAt(uri, at.line, at.character);
                }
            }
            const call = opened.at.call;
            if (call !== undefined) {
                requests["signature_help"] = () =>
                    client.signatureHelpAt(uri, call.line, call.character);
            }
            // One unmeasured warmup per request absorbs lazy first-request
            // work; all run before the measurement window opens, so the
            // server-side series carry exactly `repeats` events per kind.
            for (const [name, request] of Object.entries(requests)) {
                await bounded(request(), name);
            }
            // Log timestamps truncate to milliseconds: the window opens once
            // the wall clock leaves the warmups' last millisecond.
            const boundary = Date.now();
            while (Date.now() <= boundary) {
                // Time is the feature: the wait is for the clock itself.
                await new Promise((resolve) => setTimeout(resolve, 1));
            }
            s.perfWindowStart = boundary + 1;
            for (const [name, request] of Object.entries(requests)) {
                let answer: unknown = null;
                for (let i = 0; i < (options.repeats ?? 30); i += 1) {
                    answer = await s.measure(name, request);
                }
                s.result.sizes[name] =
                    name === "document_symbol" ? symbolCount(answer) : sizeOf(answer);
            }
        });
    }

    /// One session per server: the files opened in turn and edited twice
    /// each, then switched through twice; with `churn` a branch switch (50
    /// sources no tab shows and 10 headers the open files include changed
    /// on disk, then changed back) and the database rewritten unchanged,
    /// each until the servers settle; then every file closed.
    async session(files: readonly string[], options: { churn?: boolean } = {}): Promise<void> {
        const churned = options.churn === false ? [] : await this.churnFiles(files);
        await this.each("session", `${String(files.length)} files`, {}, async (s) => {
            const client = await s.start();
            const uris = new Map<string, string>();
            for (const file of files) {
                const { uri, ms } = await s.open(file);
                uris.set(file, uri);
                s.record("open", ms);
                await s.measure("open_semantic_tokens", () => client.semanticTokensFull(uri));
                await s.measure("open_document_symbol", () => client.documentSymbols(uri));
                for (let i = 0; i < 2; i += 1) {
                    s.record("open_edit", (await s.edit(uri)).diagnostics);
                }
            }
            // Time is the feature: what the tree holds once the opens settle.
            await pause(3000);
            s.snapshot("opened");
            for (const cycle of ["1", "2"]) {
                for (const file of shuffled(files, cycle)) {
                    const uri = uris.get(file) ?? "";
                    await s.measure("switch_semantic_tokens", () => client.semanticTokensFull(uri));
                    await s.measure("switch_inlay_hint", () =>
                        client.inlayHints(uri, {
                            start: { line: 0, character: 0 },
                            end: { line: 100, character: 0 },
                        }),
                    );
                    s.record("switch_edit", (await s.edit(uri)).diagnostics);
                }
            }
            // Time is the feature: as above, once the switches settle.
            await pause(3000);
            s.snapshot("switched");
            if (churned.length > 0) {
                await this.churn(s, churned, uris.get(files[0] ?? "") ?? "");
            }
            for (const uri of uris.values()) {
                client.close(uri);
            }
            // Time is the feature: what the tree gives back once every file
            // closed.
            await pause(15_000);
            s.snapshot("closed");
        });
    }

    /// The files a branch switch changes: 50 sources of the database no tab
    /// shows and 10 headers the open files include, none generated by the
    /// build.
    private async churnFiles(open: readonly string[]): Promise<string[]> {
        const opened = new Set(open);
        const generated = (file: string) =>
            path.resolve(this.root) !== path.resolve(this.context.database) &&
            inside(this.context.database, path.join(this.root, file)) !== null;
        const units = this.entries
            .flatMap((entry) => inside(this.root, entrySource(entry)) ?? [])
            .filter((file) => !opened.has(file) && !generated(file));
        const scanned = await scanFiles(this.root);
        const headers = scanned
            .filter((file) => opened.has(inside(this.root, file.path) ?? ""))
            .flatMap((file) => file.includes)
            .flatMap((header) => inside(this.root, header) ?? [])
            .filter((file) => !opened.has(file) && !generated(file));
        return [
            ...shuffled([...new Set(units)].sort(), "churn").slice(0, 50),
            ...shuffled([...new Set(headers)].sort(), "churn").slice(0, 10),
        ];
    }

    private notify(s: Session, files: readonly string[]): void {
        void s.client.sendNotification(proto.DidChangeWatchedFilesNotification.type, {
            changes: files.map((file) => ({
                uri: pathToFileURL(path.resolve(this.root, file)).href,
                type: proto.FileChangeType.Changed,
            })),
        });
    }

    private async churn(s: Session, files: readonly string[], first: string): Promise<void> {
        const original = new Map(
            files.map((file) => [file, fs.readFileSync(path.join(this.root, file))]),
        );
        const restore = () => {
            for (const [file, content] of original) {
                fs.writeFileSync(path.join(this.root, file), content);
            }
        };
        process.once("exit", restore);
        try {
            for (const [phase, write] of [
                [
                    "checkout",
                    (file: string) => {
                        fs.appendFileSync(path.join(this.root, file), "\n// perf churn\n");
                    },
                ],
                [
                    "checkout_back",
                    (file: string) => {
                        fs.writeFileSync(path.join(this.root, file), original.get(file) ?? "");
                    },
                ],
            ] as const) {
                const cpu = s.cpu();
                const begun = nowMs();
                for (const file of files) {
                    write(file);
                }
                this.notify(s, files);
                await s.measure(`${phase}_semantic_tokens`, () =>
                    s.client.semanticTokensFull(first),
                );
                s.record(`${phase}_edit`, (await s.edit(first)).diagnostics);
                const { idle } = await s.idle(false, SETTLE_MS);
                s.result.values[`${phase} settle s`] = round((nowMs() - begun) / 1000 - 15);
                s.result.values[`${phase} CPU s`] = round(s.cpu() - cpu);
                if (!idle) {
                    s.result.values[`${phase} unsettled`] = 1;
                }
                s.snapshot(phase);
            }
            const database = path.join(this.context.database, "compile_commands.json");
            const cpu = s.cpu();
            const begun = nowMs();
            fs.writeFileSync(database, fs.readFileSync(database));
            this.notify(s, [database]);
            await s.idle(false, SETTLE_MS);
            s.result.values["cdb_rewrite settle s"] = round((nowMs() - begun) / 1000 - 15);
            s.result.values["cdb_rewrite CPU s"] = round(s.cpu() - cpu);
            s.snapshot("cdb_rewrite");
        } finally {
            restore();
            process.off("exit", restore);
        }
    }

    /// Each file in a fresh server: clangd, clice without its PCHs and clice
    /// with them; or with `wipe: "cache"` each server without any cache and
    /// again with what that run left. The open, semantic tokens, three edits
    /// and the whole-file requests, and what the tree holds at the end.
    async sample(
        files: readonly string[],
        options: { wipe?: "pch" | "cache" } = {},
    ): Promise<void> {
        for (const file of files) {
            const variants: { servers: ServerName[]; wipe?: "pch" | "cache"; variant?: string }[] =
                options.wipe === "cache"
                    ? this.servers().flatMap((server) => [
                          { servers: [server], wipe: "cache" as const, variant: "no cache" },
                          { servers: [server] },
                      ])
                    : [
                          { servers: this.servers().filter((server) => !isClice(server)) },
                          {
                              servers: this.servers().filter(isClice),
                              wipe: "pch",
                              variant: "no PCH",
                          },
                          { servers: this.servers().filter(isClice) },
                      ];
            for (const variant of variants) {
                const label = `${String(files.length)} files`;
                await this.each("sample", label, { ...variant, file }, async (s) => {
                    const client = await s.start();
                    const { uri, ms } = await s.open(file);
                    s.record("open", ms);
                    await s.measure("semantic_tokens", () => client.semanticTokensFull(uri));
                    for (let i = 0; i < 3; i += 1) {
                        s.record("edit", (await s.edit(uri)).diagnostics);
                    }
                    await s.measure("document_symbol", () => client.documentSymbols(uri));
                    await s.measure("folding_range", () => client.foldingRanges(uri));
                    const lines = s.end(uri).line + 1;
                    await s.measure("inlay_hint", () =>
                        client.inlayHints(uri, {
                            start: { line: 0, character: 0 },
                            end: { line: lines, character: 0 },
                        }),
                    );
                    // Time is the feature: what the tree holds once it settles.
                    await pause(3000);
                    s.snapshot("end");
                });
            }
        }
    }

    /// Long editing of one file: `edits` edits, a completion at its end and
    /// a hover every tenth, what the tree holds every fiftieth.
    async soak(file: FileArg, options: { edits?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        const edits = options.edits ?? 300;
        await this.each("soak", opened.file, {}, async (s) => {
            const client = await s.start();
            const { uri } = await s.open(opened.file);
            const times: number[] = [];
            for (let i = 1; i <= edits; i += 1) {
                const { diagnostics } = await s.edit(uri);
                times.push(diagnostics);
                s.record("edit", diagnostics);
                if (i % 10 === 0) {
                    const end = s.end(uri);
                    await s.measure("completion", () =>
                        client.completionAt(uri, end.line, end.character),
                    );
                    await s.measure("hover", () =>
                        client.hoverAt(uri, Math.floor(end.line / 2), 4),
                    );
                }
                if (i % 50 === 0) {
                    s.snapshot(`edit ${String(i)}`);
                }
            }
            s.result.values["first 50 edits median ms"] = round(median(times.slice(0, 50)));
            s.result.values["last 50 edits median ms"] = round(median(times.slice(-50)));
        });
    }

    /// Requests after the server sat idle: `rounds` rounds of an edit, a
    /// hover, semantic tokens, inlay hints and a completion, each after
    /// `gap` ms of idleness.
    async idle(file: FileArg, options: { rounds?: number; gap?: number } = {}): Promise<void> {
        const opened = this.resolve(file);
        const gap = options.gap ?? 3000;
        await this.each("idle", opened.file, {}, async (s) => {
            const client = await s.start();
            const { uri } = await s.open(opened.file);
            await s.edit(uri);
            for (let i = 0; i < (options.rounds ?? 20); i += 1) {
                // Time is the feature: each request comes after the server
                // sat idle.
                await pause(gap);
                s.record("edit", (await s.edit(uri)).diagnostics);
                const end = s.end(uri);
                const middle = Math.floor(end.line / 2);
                await pause(gap);
                await s.measure("hover", () => client.hoverAt(uri, middle, 4));
                await pause(gap);
                await s.measure("semantic_tokens", () => client.semanticTokensFull(uri));
                await pause(gap);
                await s.measure("inlay_hint", () =>
                    client.inlayHints(uri, {
                        start: { line: middle, character: 0 },
                        end: { line: middle + 100, character: 0 },
                    }),
                );
                await pause(gap);
                await s.measure("completion", () =>
                    client.completionAt(uri, end.line, end.character),
                );
            }
            s.snapshot("end");
        });
    }

    /// Editing while the background index runs from scratch: every `every`
    /// ms an edit, a completion, a hover and semantic tokens, until the
    /// index has been quiet for 15 s (at most `minutes`), then ten rounds
    /// more.
    async duringIndex(
        file: FileArg,
        options: { every?: number; minutes?: number } = {},
    ): Promise<void> {
        const opened = this.resolve(file);
        const cap = (options.minutes ?? 30) * 60_000;
        await this.each("duringIndex", opened.file, { wipe: "cache" }, async (s) => {
            const client = await s.start();
            const { uri } = await s.open(opened.file);
            let lastProgress = nowMs();
            let seen = client.progressEvents.length;
            let after = 0;
            let rounds = 0;
            while (after < 10) {
                // Time is the feature: the rounds come at an editor's pace.
                await pause(options.every ?? 5000);
                if (client.progressEvents.length !== seen) {
                    seen = client.progressEvents.length;
                    lastProgress = nowMs();
                }
                const phase =
                    nowMs() - lastProgress < 15_000 && nowMs() - s.started < cap
                        ? "during"
                        : "after";
                const end = s.end(uri);
                s.record(`${phase}_edit`, (await s.edit(uri)).diagnostics);
                await s.measure(`${phase}_completion`, () =>
                    client.completionAt(uri, end.line + 1, 0),
                );
                await s.measure(`${phase}_hover`, () =>
                    client.hoverAt(uri, Math.floor(end.line / 2), 4),
                );
                await s.measure(`${phase}_semantic_tokens`, () => client.semanticTokensFull(uri));
                rounds += 1;
                if (phase === "after") {
                    after += 1;
                } else {
                    s.result.values["rounds during the index"] = rounds;
                }
                if (rounds % 20 === 0) {
                    s.snapshot(`round ${String(rounds)}`);
                }
            }
        });
    }

    /// References and rename of the symbol at `at` once the index is idle:
    /// five and three times, the sites each reaches.
    async references(file: string, at: Place): Promise<void> {
        const position = positionOf(this.root, file, at);
        await this.each("references", file, {}, async (s) => {
            const client = await s.start();
            const { uri } = await s.open(file);
            await s.idle(false);
            const { line, character } = position;
            let answer: unknown = null;
            for (let i = 0; i < 5; i += 1) {
                answer = await s.measure("references", () =>
                    client.referencesAt(uri, line, character),
                );
            }
            s.result.sizes["references"] = sizeOf(answer);
            s.result.values["reference files"] = filesOf(answer);
            let renamed: proto.WorkspaceEdit | null = null;
            for (let i = 0; i < 3; i += 1) {
                renamed = (await s.measure("rename", () =>
                    client.renameAt(uri, line, character, `renamed_${String(i)}`),
                )) as proto.WorkspaceEdit | null;
            }
            const changes = Object.values(renamed?.changes ?? {});
            const documentChanges = (renamed?.documentChanges ?? []) as proto.TextDocumentEdit[];
            s.result.values["rename files"] = changes.length + documentChanges.length;
            s.result.sizes["rename"] =
                changes.reduce((n, edits) => n + edits.length, 0) +
                documentChanges.reduce((n, change) => n + change.edits.length, 0);
        });
    }

    /// A long session cycling through the files for `minutes`: each opened
    /// when not open, edited twice and asked a completion and a hover; every
    /// fifth turn closes one of the open files.
    async long(files: readonly string[], options: { minutes?: number } = {}): Promise<void> {
        const minutes = options.minutes ?? 60;
        await this.each("long", `${String(files.length)} files`, {}, async (s) => {
            const client = await s.start();
            const open = new Map<string, string>();
            const times: Record<string, number[]> = { open: [], edit: [] };
            let turn = 0;
            let nextSnapshot = 0;
            while (nowMs() - s.started < minutes * 60_000) {
                const file = files[turn % files.length] ?? "";
                turn += 1;
                let uri = open.get(file);
                if (uri === undefined) {
                    const opened = await s.open(file);
                    uri = opened.uri;
                    open.set(file, uri);
                    times["open"]?.push(opened.ms);
                    s.record("open", opened.ms);
                }
                for (let k = 0; k < 2; k += 1) {
                    const { diagnostics } = await s.edit(uri);
                    times["edit"]?.push(diagnostics);
                    s.record("edit", diagnostics);
                }
                const end = s.end(uri);
                const current = uri;
                await s.measure("completion", () => client.completionAt(current, end.line + 1, 0));
                await s.measure("hover", () =>
                    client.hoverAt(current, Math.floor(end.line / 2), 4),
                );
                if (turn % 5 === 0 && open.size > 3) {
                    const [victim, victimUri] =
                        [...open.entries()][Math.floor(open.size / 2)] ?? [];
                    if (victim !== undefined && victimUri !== undefined) {
                        client.close(victimUri);
                        open.delete(victim);
                    }
                }
                if (nowMs() - s.started >= nextSnapshot) {
                    s.snapshot(`minute ${String(Math.round((nowMs() - s.started) / 60_000))}`);
                    nextSnapshot += 10 * 60_000;
                }
                // Time is the feature: a person's pace between turns.
                await pause(1000);
            }
            for (const [kind, values] of Object.entries(times)) {
                const half = Math.floor(values.length / 2);
                s.result.values[`${kind} first half median ms`] = round(
                    median(values.slice(0, half)),
                );
                s.result.values[`${kind} second half median ms`] = round(
                    median(values.slice(half)),
                );
            }
            s.snapshot("end");
        });
    }

    /// (clice) The dependency scan of the whole database, `runs` times cold
    /// from a fresh load and warm over what it kept.
    async scan(options: { runs?: number } = {}): Promise<void> {
        await this.each("scan", "database", { servers: ["clice"] }, async (s) => {
            const clice = this.context.binaries.clice ?? "";
            const ran = await runProcess(
                clice,
                ["scan", "--workspace", this.root, "--runs", String(options.runs ?? 20)],
                // Hang detection: twenty scans of LLVM take minutes.
                { timeout: 60 * 60_000 },
            );
            if (ran.status !== 0) {
                throw new Error(`clice scan exited ${String(ran.status)}:\n${ran.stderr}`);
            }
            for (const match of ran.stdout.matchAll(/^Run \d+: cold (\d+)ms, warm (\d+)ms$/gm)) {
                s.record("scan_cold", Number(match[1]));
                s.record("scan_warm", Number(match[2]));
            }
        });
    }
}
