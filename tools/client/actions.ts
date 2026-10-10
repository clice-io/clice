/// The serve action layer. A scenario drives a real clice server on a copy
/// of a sample project through actions of two kinds: basic ones — one LSP
/// message, one disk operation or one test-hook call — and composite ones
/// built only from basic and other composite actions. Every wait ends on a
/// server event: a reply, clice/internal/sync, a parked reply, an exit;
/// none sleeps or polls. Every action is a step of the case's record, which
/// a failing case prints together with the work the server still had.
///
/// Positions and edits are values: `at(file, anchor)` names the unique
/// snippet `anchor` of the file (its `|`, if any, marks the cursor), and a
/// Change names what it replaces by such a snippet, so neither a position
/// nor an edit spells a line or column, and the sample projects carry no
/// markers.

import { AsyncLocalStorage } from "node:async_hooks";
import * as fs from "node:fs";
import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import type {
    BuildKind,
    CurrentContextResult,
    ListConfigurationsResult,
    QueryContextResult,
    StatsResult,
    SwitchConfigurationResult,
    SwitchContextResult,
} from "../protocol/protocol.ts";
import { anomaliesInLogFiles, serverEnv } from "../process_gate.ts";
import { withTimeout } from "../promise.ts";
import type { CliceClient } from "./client.ts";
import { actionsOf, applyTextEdits } from "./edits.ts";
import { runProcess } from "./process.ts";
import { DATA_DIR, generateCDB } from "../compile_commands.ts";
import { looseManifest, materialize, writeDatabase, type Manifest } from "./project.ts";
import { cliceExecutable, type SessionFactory, type SessionOptions } from "./session.ts";
import type { Workspace } from "./workspace.ts";

/// A position: where the unique snippet `anchor` starts in the file, or
/// where its `|` stands.
export interface Loc {
    readonly file: string;
    readonly anchor: string;
}

export function at(file: string, anchor: string): Loc {
    return { file, anchor };
}

/// A change to a text, by the unique snippets it touches, taken literally;
/// `text` replaces the whole of it.
export type Change =
    | { replace: string; with: string }
    | { after: string; insert: string }
    | { before: string; insert: string }
    | { remove: string }
    | { text: string };

/// Where the one occurrence of `snippet` lies in `text`; an error naming
/// `where` when it occurs never or more than once.
export function uniqueSpan(
    text: string,
    snippet: string,
    where: string,
): { begin: number; end: number } {
    const begin = text.indexOf(snippet);
    if (begin < 0) {
        throw new Error(`${where} has no ${JSON.stringify(snippet)}`);
    }
    if (text.includes(snippet, begin + 1)) {
        throw new Error(`${where} has ${JSON.stringify(snippet)} more than once`);
    }
    return { begin, end: begin + snippet.length };
}

/// The snippet an `at()` anchor stands for, and where its cursor is in it.
export function anchorSnippet(anchor: string): { snippet: string; cursor: number } {
    const bar = anchor.indexOf("|");
    return bar < 0
        ? { snippet: anchor, cursor: 0 }
        : { snippet: anchor.slice(0, bar) + anchor.slice(bar + 1), cursor: bar };
}

/// The LSP position of `offset`: JavaScript strings index UTF-16 code
/// units, as LSP characters count.
function utf16Position(text: string, offset: number): proto.Position {
    const before = text.slice(0, offset);
    const line = before.split("\n").length - 1;
    return { line, character: offset - (before.lastIndexOf("\n") + 1) };
}

function applyChange(text: string, change: Change, where: string): string {
    if ("text" in change) {
        return change.text;
    }
    if ("replace" in change) {
        const { begin, end } = uniqueSpan(text, change.replace, where);
        return text.slice(0, begin) + change.with + text.slice(end);
    }
    if ("after" in change) {
        const { end } = uniqueSpan(text, change.after, where);
        return text.slice(0, end) + change.insert + text.slice(end);
    }
    if ("before" in change) {
        const { begin } = uniqueSpan(text, change.before, where);
        return text.slice(0, begin) + change.insert + text.slice(begin);
    }
    const { begin, end } = uniqueSpan(text, change.remove, where);
    return text.slice(0, begin) + text.slice(end);
}

function describe(change: Change): string {
    if ("text" in change) {
        return "replace the text";
    }
    if ("replace" in change) {
        return `replace ${JSON.stringify(change.replace)} with ${JSON.stringify(change.with)}`;
    }
    if ("after" in change) {
        return `insert ${JSON.stringify(change.insert)} after ${JSON.stringify(change.after)}`;
    }
    if ("before" in change) {
        return `insert ${JSON.stringify(change.insert)} before ${JSON.stringify(change.before)}`;
    }
    return `remove ${JSON.stringify(change.remove)}`;
}

function describeLoc(loc: Loc): string {
    return `${loc.file} at ${JSON.stringify(loc.anchor)}`;
}

/// The one hierarchy item a prepare request found at `loc`.
function only<T>(items: T[] | null, loc: Loc): T {
    if (items?.length !== 1) {
        throw new Error(`${describeLoc(loc)} names ${items?.length ?? 0} items, not one`);
    }
    return items[0] as T;
}

interface Step {
    depth: number;
    text: string;
    started: number;
    ended?: number;
    failed?: boolean;
}

/// The actions a case ran, a composite's own nested a level below it.
/// Each step's depth follows the async context it runs in, so actions
/// awaited side by side nest under their own callers.
class StepRecord {
    private steps: Step[] = [];
    private readonly depth = new AsyncLocalStorage<number>();

    /// Run `body` as the step `text`.
    run<T>(text: string, body: () => Promise<T>): Promise<T> {
        const depth = this.depth.getStore() ?? 0;
        const step: Step = { depth, text, started: Date.now() };
        this.steps.push(step);
        return this.depth.run(depth + 1, async () => {
            try {
                return await body();
            } catch (error) {
                step.failed = true;
                throw error;
            } finally {
                step.ended = Date.now();
            }
        });
    }

    /// A step that sends and waits for nothing.
    note(text: string): void {
        const now = Date.now();
        this.steps.push({ depth: this.depth.getStore() ?? 0, text, started: now, ended: now });
    }

    render(): string {
        const now = Date.now();
        const width = String(this.steps.length).length;
        return this.steps
            .map((step, index) => {
                const head = `step ${String(index + 1).padStart(width)}/${this.steps.length}  ${"  ".repeat(step.depth)}${step.text}`;
                if (step.ended === undefined) {
                    return `${head} ... no reply after ${Math.round((now - step.started) / 1000)} s`;
                }
                return step.failed === true ? `${head} ... failed` : head;
            })
            .join("\n");
    }
}

/// A file's text, or what makes it from the workspace (a database naming
/// its absolute paths).
export type FileText = string | ((workspace: Workspace) => string);

export interface ServeOptions {
    /// initializationOptions over the test defaults, shaped as clice.toml.
    config?: Record<string, unknown>;
    /// Files written over the project before the server starts; the whole
    /// workspace of a case without a project.
    files?: Record<string, FileText>;
    /// A request of a file, as `{ request: "tuRun", file: "src/registry.cpp" }`:
    /// the worker running the first such request dies the way a kill from
    /// outside ends it, naming no request (CLICE_TEST_KILL_REQUEST). The case
    /// ends checking that the kill happened and that the one worker crash it
    /// reports is the only anomaly.
    killOn?: { request: string; file: string };
    /// Every request of a file named so crashes its worker the way a fault
    /// does, attributed to the request (CLICE_TEST_CRASH_REQUEST). The case
    /// asserts the crashes it expects itself.
    crashOn?: { request: string; file: string };
    /// The units and arguments of a case without a project, in place of
    /// every source a unit with the default arguments.
    manifest?: Manifest;
    /// A data workspace (tests/data/<data>) copied as the case's workspace,
    /// its database generated when it has a CMakeLists.txt. A bridge for
    /// cases not yet on a sample project.
    data?: string;
    /// Environment of the server over the test defaults.
    env?: Record<string, string>;
    /// The case crashes or misleads the server on purpose: anomalies do not
    /// fail it, and Debug builds do not trap on them. The case asserts the
    /// anomalies it expects itself.
    anomalies?: boolean;
    /// The compilation databases written before the server starts, by
    /// workspace-relative path, in place of the one at the root the
    /// manifest describes; false for none.
    databases?: false | Record<string, Manifest>;
    /// A step on the workspace before the first server starts, after the
    /// files and databases are written: what they cannot hold, such as a
    /// symlink.
    setup?: (workspace: Workspace) => void;
    /// How every server of the case starts, unless `s.start` says otherwise.
    launch?: Launch;
}

/// How a server starts, beyond the case's environment: its config in place
/// of the case's, client capabilities, the workspace folders it announces
/// (workspace-relative; null announces none, only the root), a step between
/// the initialize response and the initialized notification (`s.client` is
/// the new server then), the program and its arguments. With
/// `handshake: false` the server is only spawned: the case sends initialize
/// itself through `s.client`.
/// `${workspace}` in the program, an argument or an environment value
/// stands for the workspace root.
export type Launch = Pick<
    SessionOptions,
    "capabilities" | "folders" | "args" | "executable" | "drainStderr"
> & {
    config?: Record<string, unknown>;
    handshake?: false;
    beforeInitialized?: (s: Serve) => void | Promise<void>;
};

/// A hold on the next reply of a build (clice/internal/hold).
export interface Hold {
    readonly id: number;
    /// Resolves once the hold parked a reply.
    reached(): Promise<void>;
    /// Lets the parked reply go on, or drops a hold no reply reached.
    release(): Promise<void>;
}

interface BuildCounts {
    compile: number;
    pch: number;
    pcm: number;
    index: number;
}

/// The builds the server ran, in all and per file, with the diagnostics
/// publishes each file received.
export interface Counts extends BuildCounts {
    files: Record<string, BuildCounts & { publish: number }>;
}

export interface SyncState {
    /// Workspace-relative files whose latest index attempt failed for good.
    failed: string[];
    /// Index state remains that no save committed.
    unsaved: boolean;
}

export interface CliRun {
    status: number | null;
    stdout: string;
    stderr: string;
    /// stdout parsed, when it is JSON.
    json: unknown;
}

interface Document {
    version: number;
    text: string;
}

/// One case's server and workspace; `s` in a scenario.
export class Serve {
    readonly steps = new StepRecord();
    /// The sample project the workspace is a copy of; null for loose files.
    readonly project: string | null;
    readonly workspace: Workspace;
    readonly manifest: Manifest;

    /// The session the case's servers come from: an escape hatch for what
    /// the actions do not express (a second server, custom arguments).
    readonly session: SessionFactory;
    private readonly options: ServeOptions;
    private server: CliceClient | null = null;
    private readonly documents = new Map<string, Document>();

    private constructor(
        session: SessionFactory,
        project: string | null,
        workspace: Workspace,
        manifest: Manifest,
        options: ServeOptions,
    ) {
        this.session = session;
        this.project = project;
        this.workspace = workspace;
        this.manifest = manifest;
        this.options = options;
    }

    /// A copy of `project` (or of the loose `options.files`) with a server
    /// started on it.
    static async create(
        session: SessionFactory,
        project: string | null,
        options: ServeOptions,
    ): Promise<Serve> {
        const workspace = session.tmpdir();
        const files = options.files ?? {};
        let manifest: Manifest;
        let databases: Record<string, Manifest> = {};
        if (project !== null) {
            manifest = materialize(project, workspace);
            databases = { "compile_commands.json": manifest };
        } else if (options.data !== undefined) {
            fs.cpSync(path.join(DATA_DIR, options.data), workspace.root, { recursive: true });
            // What runs in the data directory itself left there.
            workspace.rm(".clice");
            workspace.rm("build");
            if (workspace.exists("CMakeLists.txt")) {
                generateCDB(workspace.root);
            }
            manifest = { units: {} };
        } else {
            manifest = options.manifest ?? looseManifest(Object.keys(files));
            databases = { "compile_commands.json": manifest };
        }
        for (const [file, text] of Object.entries(files)) {
            workspace.write(file, typeof text === "string" ? text : text(workspace));
        }
        if (options.databases !== undefined) {
            databases = options.databases === false ? {} : options.databases;
        }
        for (const [at, described] of Object.entries(databases)) {
            writeDatabase(workspace, described, at);
        }
        options.setup?.(workspace);
        const s = new Serve(session, project, workspace, manifest, options);
        if (options.killOn !== undefined) {
            const { request, file } = options.killOn;
            workspace.write(s.killFile(), `${request} ${workspace.displayPath(file)}`);
        }
        await s.start();
        return s;
    }

    /// The file a logical name of the manifest stands for.
    file(name: string): string {
        const file = this.manifest.files?.[name];
        if (file === undefined) {
            throw new Error(
                `${this.project ?? "the workspace"} names no file ${JSON.stringify(name)}`,
            );
        }
        return file;
    }

    uri(file: string): string {
        return this.workspace.uri(file);
    }

    /// The workspace-relative path of a URI the server sent; an absolute
    /// one for a file outside the workspace.
    relative(uri: string): string {
        const file = URI.parse(uri).fsPath;
        const relative = path.relative(this.workspace.root, file);
        return relative.startsWith("..") ? file : relative.split(path.sep).join("/");
    }

    /// didOpen with the file's disk text, or `text` for a buffer the disk
    /// does not hold, at `version`; then a request a compile starts on,
    /// unless `pull` is false. Nothing waits.
    open(file: string, options: { text?: string; version?: number; pull?: false } = {}): void {
        this.openWith(file, options.text ?? this.workspace.read(file), options);
    }

    edit(file: string, ...changes: Change[]): void {
        const document = this.document(file);
        let text = document.text;
        for (const change of changes) {
            text = applyChange(text, change, file);
        }
        document.version += 1;
        document.text = text;
        this.steps.note(`edit ${file} v${document.version}: ${changes.map(describe).join("; ")}`);
        this.live().change(this.uri(file), document.version, text);
    }

    /// Write the buffer to disk and say so (didSave). A file not open, or
    /// one saved with `write: false` (a save hook rewrote the disk), is
    /// announced with the disk as it stands.
    save(file: string, options: { write?: false } = {}): void {
        const document = this.documents.get(file);
        if (document !== undefined && options.write !== false) {
            this.steps.note(`save ${file} v${document.version}`);
            this.workspace.write(file, document.text);
        } else {
            this.steps.note(`save ${file} as the disk holds it`);
        }
        this.live().save(this.uri(file));
    }

    close(file: string): void {
        this.document(file);
        this.steps.note(`close ${file}`);
        this.documents.delete(file);
        this.live().close(this.uri(file));
    }

    /// The disk under the workspace; no message is sent.
    readonly disk = {
        read: (file: string): string => this.workspace.read(file),
        write: (file: string, text: string): void => {
            this.steps.note(`disk: write ${file}`);
            this.workspace.write(file, text);
        },
        edit: (file: string, ...changes: Change[]): void => {
            this.steps.note(`disk: edit ${file}: ${changes.map(describe).join("; ")}`);
            let text = this.workspace.read(file);
            for (const change of changes) {
                text = applyChange(text, change, file);
            }
            this.workspace.write(file, text);
        },
        rm: (file: string): void => {
            this.steps.note(`disk: remove ${file}`);
            fs.rmSync(this.workspace.path(file));
        },
        /// Write the compilation database `manifest` describes at `at`.
        database: (manifest: Manifest, at = "compile_commands.json"): void => {
            const units = Object.keys(manifest.units).join(", ");
            this.steps.note(`disk: ${at} of ${units === "" ? "no unit" : units}`);
            writeDatabase(this.workspace, manifest, at);
        },
    };

    /// The diagnostics of `file` as it stands, as the push to the editor
    /// carries them: a pull (textDocument/diagnostic) waits for the compile
    /// of the version sent last, which a hover answered from the index
    /// would not, and its answer is the push's content.
    diagnostics(file: string): Promise<proto.Diagnostic[]> {
        const version = this.document(file).version;
        return this.ask(`diagnostics ${file} v${version}`, (client) =>
            client.pullDiagnostics(this.uri(file)),
        );
    }

    /// The diagnostics last pushed for `file`, open or not, once the
    /// server settled; undefined when none was. Unlike `diagnostics`, it
    /// starts no compile: what it reads is what the server did on its own.
    pushed(file: string): Promise<proto.Diagnostic[] | undefined> {
        return this.steps.run(`pushed ${file}`, async () => {
            await this.sync();
            return this.live().lastPublish(this.uri(file))?.diagnostics;
        });
    }

    /// Every diagnostics push `file` received, oldest first, once the
    /// server settled.
    pushes(file: string): Promise<proto.Diagnostic[][]> {
        return this.steps.run(`pushes of ${file}`, async () => {
            await this.sync();
            const client = this.live();
            const uri = client.normalizeUri(this.uri(file));
            return client.publishedDiagnostics
                .filter((params) => client.normalizeUri(params.uri) === uri)
                .map((params) => params.diagnostics);
        });
    }

    /// How many `method` requests the server sent the client (a refresh, a
    /// workspace/configuration), once it settled.
    serverRequests(method: string): Promise<number> {
        return this.steps.run(`${method} requests from the server`, async () => {
            await this.sync();
            return this.live().serverRequests.filter((sent) => sent === method).length;
        });
    }

    hover(loc: Loc): Promise<proto.Hover | null> {
        const { uri, position } = this.position(loc);
        return this.ask(`hover ${describeLoc(loc)}`, (client) =>
            client.hoverAt(uri, position.line, position.character),
        );
    }

    definition(loc: Loc): Promise<proto.Definition | proto.LocationLink[] | null> {
        const { uri, position } = this.position(loc);
        return this.ask(`definition ${describeLoc(loc)}`, (client) =>
            client.definitionAt(uri, position.line, position.character),
        );
    }

    references(loc: Loc): Promise<proto.Location[] | null> {
        const { uri, position } = this.position(loc);
        return this.ask(`references ${describeLoc(loc)}`, (client) =>
            client.referencesAt(uri, position.line, position.character),
        );
    }

    /// The code actions at `loc`, or over the anchor's whole snippet with
    /// `span`; `only` filters them by kind as an editor asks.
    codeActions(
        loc: Loc,
        options: { only?: string[]; span?: true } = {},
    ): Promise<(proto.Command | proto.CodeAction)[] | null> {
        const { uri, position } = this.position(loc);
        const range = options.span === true ? this.range(loc) : { start: position, end: position };
        const only = options.only === undefined ? "" : ` only ${options.only.join(", ")}`;
        return this.ask(`codeActions ${describeLoc(loc)}${only}`, (client) =>
            client.sendRequest(proto.CodeActionRequest.type, {
                textDocument: { uri },
                range,
                context: {
                    diagnostics: [],
                    ...(options.only === undefined ? {} : { only: options.only }),
                },
            }),
        );
    }

    /// Completion at `loc`, as typing `trigger` asks for it when given.
    completion(
        loc: Loc,
        trigger?: string,
    ): Promise<proto.CompletionItem[] | proto.CompletionList | null> {
        const { uri, position } = this.position(loc);
        const by = trigger === undefined ? "" : ` triggered by ${JSON.stringify(trigger)}`;
        return this.ask(`completion ${describeLoc(loc)}${by}`, (client) =>
            client.completionAt(uri, position.line, position.character, {
                ...(trigger === undefined ? {} : { triggerCharacter: trigger }),
            }),
        );
    }

    signatureHelp(loc: Loc): Promise<proto.SignatureHelp | null> {
        const { uri, position } = this.position(loc);
        return this.ask(`signatureHelp ${describeLoc(loc)}`, (client) =>
            client.signatureHelpAt(uri, position.line, position.character),
        );
    }

    /// The calls into the one function at `loc`.
    incomingCalls(loc: Loc): Promise<proto.CallHierarchyIncomingCall[] | null> {
        return this.ask(`incomingCalls ${describeLoc(loc)}`, async (client) => {
            const { uri, position } = this.position(loc);
            const item = only(
                await client.prepareCallHierarchy(uri, position.line, position.character),
                loc,
            );
            return client.callHierarchyIncoming(item);
        });
    }

    /// The calls out of the one function at `loc`.
    outgoingCalls(loc: Loc): Promise<proto.CallHierarchyOutgoingCall[] | null> {
        return this.ask(`outgoingCalls ${describeLoc(loc)}`, async (client) => {
            const { uri, position } = this.position(loc);
            const item = only(
                await client.prepareCallHierarchy(uri, position.line, position.character),
                loc,
            );
            return client.callHierarchyOutgoing(item);
        });
    }

    /// The direct bases of the one type at `loc`.
    supertypes(loc: Loc): Promise<proto.TypeHierarchyItem[] | null> {
        return this.ask(`supertypes ${describeLoc(loc)}`, async (client) => {
            const { uri, position } = this.position(loc);
            const item = only(
                await client.prepareTypeHierarchy(uri, position.line, position.character),
                loc,
            );
            return client.typeHierarchySupertypes(item);
        });
    }

    /// The direct derived types of the one type at `loc`.
    subtypes(loc: Loc): Promise<proto.TypeHierarchyItem[] | null> {
        return this.ask(`subtypes ${describeLoc(loc)}`, async (client) => {
            const { uri, position } = this.position(loc);
            const item = only(
                await client.prepareTypeHierarchy(uri, position.line, position.character),
                loc,
            );
            return client.typeHierarchySubtypes(item);
        });
    }

    /// The lines of `file` the server marks inactive (the semantic tokens'
    /// `inactive` modifier), counted from 0.
    inactiveLines(file: string): Promise<number[]> {
        return this.ask(`inactive lines of ${file}`, (client) =>
            client.inactiveLines(this.uri(file)),
        );
    }

    /// The header contexts `file` can take (clice/queryContext), from its
    /// `offset`-th one on.
    contexts(file: string, options: { offset?: number } = {}): Promise<QueryContextResult> {
        return this.ask(`contexts of ${file}`, (client) =>
            client.queryContext(this.uri(file), options),
        );
    }

    currentContext(file: string): Promise<CurrentContextResult> {
        return this.ask(`current context of ${file}`, (client) =>
            client.currentContext(this.uri(file)),
        );
    }

    /// Compile `file` in the context of its includer `host` (itself for a
    /// command of its own), at its `occurrence`-th include there, under the
    /// command of `commandHash`; `epoch` stamps the listing it came from.
    switchContext(
        file: string,
        host: string,
        options: { occurrence?: number; commandHash?: string; epoch?: number } = {},
    ): Promise<SwitchContextResult> {
        return this.ask(`switch ${file} to the context of ${host}`, (client) =>
            client.switchContext(this.uri(file), this.uri(host), options),
        );
    }

    resetContext(file: string): Promise<SwitchContextResult> {
        return this.ask(`reset the context of ${file}`, (client) =>
            client.resetContext(this.uri(file)),
        );
    }

    /// The build configurations of the project serving `file`, else of the
    /// first project.
    configurations(file?: string): Promise<ListConfigurationsResult> {
        return this.ask(`configurations${file === undefined ? "" : ` of ${file}`}`, (client) =>
            client.listConfigurations(file === undefined ? undefined : this.uri(file)),
        );
    }

    switchConfiguration(name: string, file?: string): Promise<SwitchConfigurationResult> {
        const of = file === undefined ? "" : ` for ${file}`;
        return this.ask(`switch to the configuration ${JSON.stringify(name)}${of}`, (client) =>
            client.switchConfiguration(name, file === undefined ? undefined : this.uri(file)),
        );
    }

    workspaceSymbols(
        query: string,
    ): Promise<proto.SymbolInformation[] | proto.WorkspaceSymbol[] | null> {
        return this.ask(`workspaceSymbols ${JSON.stringify(query)}`, (client) =>
            client.workspaceSymbols(query),
        );
    }

    /// Any request about a document, at a position when `where` is a Loc;
    /// `extra` joins the parameters. The caller names the reply's type.
    request<T = unknown>(method: string, where: string | Loc, extra: object = {}): Promise<T> {
        return this.send<T>(method, where, extra).reply;
    }

    /// A request as `request` sends it, with the means to cancel it
    /// ($/cancelRequest) while it is under way.
    send<T = unknown>(
        method: string,
        where: string | Loc,
        extra: object = {},
    ): { reply: Promise<T>; cancel(): void } {
        let target: object;
        if (typeof where === "string") {
            target = { textDocument: { uri: this.uri(where) } };
        } else {
            const { uri, position } = this.position(where);
            target = { textDocument: { uri }, position };
        }
        const name = typeof where === "string" ? where : describeLoc(where);
        const source = new proto.CancellationTokenSource();
        const reply = this.ask(
            `${method} ${name}`,
            (client) =>
                client.sendRequest(method, { ...target, ...extra }, source.token) as Promise<T>,
        );
        return {
            reply,
            cancel: () => {
                this.steps.note(`cancel ${method} ${name}`);
                source.cancel();
            },
        };
    }

    /// The LSP position of `loc` in the text requests about its file see.
    position(loc: Loc): { uri: string; position: proto.Position } {
        const text = this.text(loc.file);
        const { snippet, cursor } = anchorSnippet(loc.anchor);
        const { begin } = uniqueSpan(text, snippet, loc.file);
        return { uri: this.uri(loc.file), position: utf16Position(text, begin + cursor) };
    }

    /// The range the snippet of `loc` covers.
    range(loc: Loc): proto.Range {
        const text = this.text(loc.file);
        const { begin, end } = uniqueSpan(text, anchorSnippet(loc.anchor).snippet, loc.file);
        return { start: utf16Position(text, begin), end: utf16Position(text, end) };
    }

    /// Wait until the server has no work left (clice/internal/sync) — after
    /// a look at the disk with `poll`; the server answering with work still
    /// pending at its deadline fails the step with that work.
    sync(options: { poll?: boolean } = {}): Promise<SyncState> {
        return this.ask(options.poll === true ? "sync after a poll" : "sync", async (client) => {
            const result = await client.sync(options);
            if (result.pending.length > 0) {
                throw new Error(`the server did not settle:\n${result.pending.join("\n")}`);
            }
            return {
                failed: result.failed.map((uri) => this.relative(uri)),
                unsaved: result.unsaved,
            };
        });
    }

    counts(): Promise<Counts> {
        return this.ask("counts", async (client) => {
            const counts: Counts = { compile: 0, pch: 0, pcm: 0, index: 0, files: {} };
            for (const build of (await client.stats()).builds) {
                counts.files[this.relative(build.uri)] = {
                    compile: build.compile,
                    pch: build.pch,
                    pcm: build.pcm,
                    index: build.index,
                    publish: client.publishCount(build.uri),
                };
                counts.compile += build.compile;
                counts.pch += build.pch;
                counts.pcm += build.pcm;
                counts.index += build.index;
            }
            for (const file of this.documents.keys()) {
                counts.files[file] ??= {
                    compile: 0,
                    pch: 0,
                    pcm: 0,
                    index: 0,
                    publish: client.publishCount(this.uri(file)),
                };
            }
            return counts;
        });
    }

    /// The server's whole clice/internal/stats: its gauges (sessions,
    /// shards, contexts, tmp files) beside the build counts.
    stats(): Promise<StatsResult> {
        return this.ask("stats", (client) => client.stats());
    }

    /// Look at the disk once (clice/internal/poll): the database (`force`
    /// looks past its stamp) or every known file of the workspace. Returns
    /// the number of file events the look produced.
    poll(loop: "cdb" | "workspace", options: { force?: boolean } = {}): Promise<number> {
        const forced = options.force === true ? " forced" : "";
        return this.ask(
            `poll ${loop}${forced}`,
            async (client) => (await client.poll(loop, options)).events,
        );
    }

    /// The server logged no anomaly so far.
    noAnomaly(): Promise<void> {
        return this.ask("no anomaly so far", (client) => {
            client.assertNoAnomaly();
            return Promise.resolve();
        });
    }

    /// The running server's initialize result.
    get initResult(): proto.InitializeResult {
        const result = this.live().initResult;
        if (result === null) {
            throw new Error("the server has not been initialized");
        }
        return result;
    }

    /// Announce workspace folders, workspace-relative, coming and going
    /// (didChangeWorkspaceFolders).
    changeFolders(change: { added?: string[]; removed?: string[] }): Promise<void> {
        const parts = [
            ...(change.added ?? []).map((folder) => `+${folder}`),
            ...(change.removed ?? []).map((folder) => `-${folder}`),
        ];
        return this.ask(`folders ${parts.join(" ")}`, (client) =>
            client.changeWorkspaceFolders(change),
        );
    }

    /// Kill the server from outside (SIGKILL) and wait for it to be gone:
    /// whatever it had not committed stays as it was. The documents it had
    /// open are gone with it.
    kill(): Promise<void> {
        return this.steps.run("kill the server", async () => {
            const client = this.live();
            client.killServer();
            await client.exited;
            client.dispose();
            this.server = null;
            this.documents.clear();
        });
    }

    /// The first of `holds` to park a reply; the others may still park one
    /// or go with the server.
    firstParked(holds: Hold[]): Promise<Hold> {
        return this.steps.run(
            `first of holds ${holds.map((hold) => hold.id).join(", ")} to park`,
            () =>
                Promise.any(
                    holds.map(async (hold) => {
                        await hold.reached();
                        return hold;
                    }),
                ),
        );
    }

    /// Hold the next reply of a `kind` build of `file`; resolves once the
    /// hold is in place.
    async hold(kind: BuildKind, file: string): Promise<Hold> {
        const id = await this.ask(`hold ${kind} ${file}`, (client) =>
            client.hold(kind, this.uri(file)),
        );
        return {
            id,
            reached: () =>
                this.ask(`hold ${id} parks the ${kind} of ${file}`, (client) =>
                    client.parkedBy(id),
                ),
            release: () => this.ask(`release hold ${id}`, (client) => client.release(id)),
        };
    }

    /// Park the work of the next `request` for `file` in its worker, where
    /// the work starts (clice/internal/gate): the worker is busy with it, so
    /// a cancel, an edit or a death lands mid-work. `request` names the
    /// worker request as killOn does ("compile", "completion", "tuRun",
    /// "query:Hover"). Resolves once the gate is in place.
    async gate(request: string, file: string): Promise<Hold> {
        const id = await this.ask(`gate ${request} ${file}`, (client) =>
            client.gate(request, this.uri(file)),
        );
        return {
            id,
            reached: () =>
                this.ask(`gate ${id} parks the ${request} of ${file} in its worker`, (client) =>
                    client.parkedBy(id),
                ),
            release: () => this.ask(`release gate ${id}`, (client) => client.release(id)),
        };
    }

    /// Run `body` while the work of a `request` for `file` is parked in its
    /// worker, as `inFlight` does for a build's reply: the gate is placed,
    /// `begin` sends the request, `body` runs once the work is parked, and
    /// the work goes on after `body`, failing or not.
    inWorker<T>(
        request: string,
        file: string,
        begin: () => void,
        body: () => T | Promise<T>,
    ): Promise<T> {
        return this.steps.run(`while the ${request} of ${file} is in its worker`, async () => {
            const gate = await this.gate(request, file);
            begin();
            await gate.reached();
            try {
                return await body();
            } finally {
                await gate.release();
            }
        });
    }

    /// Start a server on the workspace; its cache is the one a server
    /// before it left. `launch` holds for this server only, in place of the
    /// case's.
    start(launch: Launch = this.options.launch ?? {}): Promise<void> {
        return this.steps.run("start the server", async () => {
            const root = this.workspace.root;
            const expand = (text: string): string => text.replaceAll("${workspace}", root);
            const env: Record<string, string> = Object.fromEntries(
                Object.entries(this.options.env ?? {}).map(([key, value]) => [key, expand(value)]),
            );
            const { killOn, crashOn } = this.options;
            const anomalies =
                this.options.anomalies === true || killOn !== undefined || crashOn !== undefined;
            if (anomalies) {
                // A Debug build traps on an anomaly.
                env["CLICE_ANOMALY_NO_TRAP"] = "1";
            }
            if (killOn !== undefined) {
                env["CLICE_TEST_KILL_REQUEST"] = this.workspace.path(this.killFile());
            }
            if (crashOn !== undefined) {
                env["CLICE_TEST_CRASH_REQUEST"] =
                    `${crashOn.request} ${this.workspace.displayPath(crashOn.file)}`;
            }
            const { config, handshake, capabilities, folders, beforeInitialized, ...spawn } =
                launch;
            const client = this.session.spawn(this.workspace, {
                ...spawn,
                ...(spawn.executable === undefined ? {} : { executable: expand(spawn.executable) }),
                ...(spawn.args === undefined ? {} : { args: spawn.args.map(expand) }),
                env,
                allowAnomaly: anomalies,
            });
            this.server = client;
            if (handshake === false) {
                return;
            }
            await client.initialize(this.workspace, {
                initializationOptions: config ?? this.options.config,
                capabilities,
                folders,
                ...(beforeInitialized === undefined
                    ? {}
                    : { beforeInitialized: async () => beforeInitialized(this) }),
            });
        });
    }

    /// Shut the server down through its exit gate; the documents it had
    /// open are gone with it.
    stop(): Promise<void> {
        return this.steps.run("stop the server", async () => {
            await this.live().shutdown();
            this.server = null;
            this.documents.clear();
        });
    }

    /// Run a clice command on the workspace (`--workspace` follows the
    /// command's name) until it exits.
    cli(command: string, ...args: string[]): Promise<CliRun> {
        return this.steps.run(`clice ${[command, ...args].join(" ")}`, async () => {
            const run = await runProcess(
                cliceExecutable(),
                [command, "--workspace", this.workspace.root, ...args],
                { env: serverEnv() },
            );
            let json: unknown;
            try {
                json = JSON.parse(run.stdout);
            } catch {
                // Not JSON.
            }
            return { status: run.status, stdout: run.stdout, stderr: run.stderr, json };
        });
    }

    /// A reply rendered for comparison: a location as `file: its line`, a
    /// hover as its text, a symbol as its name and location; a list one
    /// item a line.
    show(value: unknown): string {
        if (value === null || value === undefined) {
            return "";
        }
        if (Array.isArray(value)) {
            return value.map((item) => this.show(item)).join("\n");
        }
        // Before Location: proto.Location.is takes a hover's range with no
        // uri for one.
        const hover = value as Partial<proto.Hover>;
        if (hover.contents !== undefined) {
            const contents = Array.isArray(hover.contents) ? hover.contents : [hover.contents];
            return contents
                .map((part) => (typeof part === "string" ? part : part.value))
                .join("\n");
        }
        if (proto.Location.is(value)) {
            return this.showLocation(value.uri, value.range);
        }
        if (proto.LocationLink.is(value)) {
            return this.showLocation(value.targetUri, value.targetSelectionRange);
        }
        const symbol = value as Partial<proto.SymbolInformation>;
        if (symbol.name !== undefined && symbol.location !== undefined) {
            return `${symbol.name} ${this.showLocation(symbol.location.uri, symbol.location.range)}`;
        }
        throw new Error(`no rendering for ${JSON.stringify(value)}`);
    }

    /// The diagnostics of `file` as it stands, opening it first if needed.
    compiled(file: string): Promise<proto.Diagnostic[]> {
        return this.steps.run(`compiled ${file}`, () => {
            if (!this.documents.has(file)) {
                this.open(file);
            }
            return this.diagnostics(file);
        });
    }

    /// `file` compiles without a diagnostic.
    clean(file: string): Promise<void> {
        return this.steps.run(`clean ${file}`, async () => {
            const diagnostics = await this.compiled(file);
            if (diagnostics.length > 0) {
                const lines = diagnostics.map(
                    (d) =>
                        `${d.range.start.line + 1}: ${typeof d.message === "string" ? d.message : d.message.value}`,
                );
                throw new Error(`${file} does not compile clean:\n${lines.join("\n")}`);
            }
        });
    }

    /// The errors `file` compiles with.
    errors(file: string): Promise<proto.Diagnostic[]> {
        return this.steps.run(`errors ${file}`, async () =>
            (await this.compiled(file)).filter(
                (diagnostic) => diagnostic.severity === proto.DiagnosticSeverity.Error,
            ),
        );
    }

    /// The server settled with every unit indexed.
    indexed(): Promise<void> {
        return this.steps.run("indexed", async () => {
            const { failed } = await this.sync();
            if (failed.length > 0) {
                throw new Error(`units failed to index: ${failed.join(", ")}`);
            }
        });
    }

    /// Apply a workspace edit as an editor does: to the buffer of an open
    /// file, to the disk of another. Returns the new text of each file.
    apply(edit: proto.WorkspaceEdit): Promise<Record<string, string>> {
        return this.steps.run("apply a workspace edit", () => {
            const changes = new Map<string, proto.TextEdit[]>();
            for (const change of edit.documentChanges ?? []) {
                if ("textDocument" in change) {
                    // An editor refuses an edit computed for another version
                    // of the buffer.
                    const file = this.relative(change.textDocument.uri);
                    const { version } = change.textDocument;
                    if (version !== null && version !== this.documents.get(file)?.version) {
                        throw new Error(
                            `the edit is for ${file} v${version}, which is not the open buffer`,
                        );
                    }
                    changes.set(
                        change.textDocument.uri,
                        change.edits.filter((item): item is proto.TextEdit => "newText" in item),
                    );
                }
            }
            for (const [uri, edits] of Object.entries(edit.changes ?? {})) {
                changes.set(uri, edits);
            }
            const texts: Record<string, string> = {};
            for (const [uri, edits] of changes) {
                const file = this.relative(uri);
                const text = applyTextEdits(this.text(file), edits);
                if (this.documents.has(file)) {
                    this.edit(file, { text });
                } else {
                    this.disk.write(file, text);
                }
                texts[file] = text;
            }
            return Promise.resolve(texts);
        });
    }

    /// Apply the code action titled `title` at `loc`; returns the file's
    /// new text and the diagnostics it compiles with.
    applyAction(
        loc: Loc,
        title: string,
    ): Promise<{ text: string; diagnostics: proto.Diagnostic[] }> {
        return this.steps.run(`apply ${JSON.stringify(title)} at ${describeLoc(loc)}`, async () => {
            const actions = actionsOf(await this.codeActions(loc));
            const action = actions.find((candidate) => candidate.title === title);
            if (action?.edit === undefined) {
                const offered = actions.map((a) => JSON.stringify(a.title)).join(", ");
                throw new Error(
                    `no action ${JSON.stringify(title)} with an edit; offered: ${offered}`,
                );
            }
            await this.apply(action.edit);
            return { text: this.text(loc.file), diagnostics: await this.compiled(loc.file) };
        });
    }

    /// Stop and start the server, the cache kept, and open again the
    /// documents it had open, their buffers kept.
    restart(): Promise<void> {
        return this.steps.run("restart", async () => {
            const documents = [...this.documents].map(([file, { text }]) => ({ file, text }));
            await this.stop();
            await this.start();
            for (const { file, text } of documents) {
                this.openWith(file, text);
            }
        });
    }

    /// Run `body` with the server down — changes it makes are ones no
    /// server saw happen — and start a server again, `body` failing or not.
    offline(body: () => void | Promise<void>): Promise<void> {
        return this.steps.run("offline", async () => {
            await this.stop();
            try {
                await body();
            } finally {
                await this.start();
            }
        });
    }

    /// Run `body` while a `kind` build of `file` is in flight: the hold is
    /// placed, `begin` starts the build — none of it may be under way
    /// before, or its reply is the one parked — and `body` runs once the
    /// reply is parked; the reply goes on after `body`, failing or not.
    /// What `body` returns is returned: a promise wrapped in an object, or
    /// it would be awaited before the build goes on.
    inFlight<T>(
        kind: BuildKind,
        file: string,
        begin: () => void,
        body: () => T | Promise<T>,
    ): Promise<T> {
        return this.steps.run(`while the ${kind} of ${file} is in flight`, async () => {
            const hold = await this.hold(kind, file);
            begin();
            await hold.reached();
            try {
                return await body();
            } finally {
                await hold.release();
            }
        });
    }

    /// Stop the server, remove its cache — the logs stay — and start one
    /// again.
    cold(): Promise<void> {
        return this.steps.run("cold start", async () => {
            await this.stop();
            this.workspace.rm(".clice/cache");
            await this.start();
        });
    }

    /// End the case before the session's teardown: a failed case prints its
    /// steps and the work the server still had; the server is shut down
    /// here so a killOn case can check the logs it left.
    async finish(failed: boolean): Promise<void> {
        const client = this.server;
        if (failed) {
            console.log(`steps:\n${this.steps.render()}`);
            if (client !== null && !client.disposed) {
                try {
                    const { pending } = await withTimeout(
                        client.sync({ deadlineMs: 1_000 }),
                        10_000,
                        "sync",
                    );
                    console.log(
                        pending.length > 0 ? `server busy:\n${pending.join("\n")}` : "server idle",
                    );
                } catch (error) {
                    console.log(`no answer to sync: ${String(error)}`);
                }
            }
        }
        if (client !== null && !client.disposed) {
            await client.shutdown({ verbose: failed });
        }
        const killOn = this.options.killOn;
        if (killOn !== undefined) {
            if (!this.workspace.exists(`${this.killFile()}.taken`)) {
                throw new Error(`killOn: no worker ran ${killOn.request} ${killOn.file}`);
            }
            const anomalies = anomaliesInLogFiles(this.workspace.root);
            if (anomalies.length !== 1 || anomalies[0]?.startsWith("WorkerCrash ") !== true) {
                throw new Error(
                    `killOn: one worker crash expected, the logs have:\n${anomalies.join("\n")}`,
                );
            }
        }
    }

    /// The running server's client: an escape hatch for messages the
    /// actions do not send (raw protocol, a notification of its own).
    get client(): CliceClient {
        return this.live();
    }

    private live(): CliceClient {
        if (this.server === null) {
            throw new Error("the server is stopped");
        }
        return this.server;
    }

    private document(file: string): Document {
        const document = this.documents.get(file);
        if (document === undefined) {
            throw new Error(`${file} is not open`);
        }
        return document;
    }

    /// The text requests about `file` see: its buffer, else its disk.
    private text(file: string): string {
        return this.documents.get(file)?.text ?? this.workspace.read(file);
    }

    private openWith(
        file: string,
        text: string,
        options: { version?: number; pull?: false } = {},
    ): void {
        const version = options.version ?? 0;
        this.steps.note(`open ${file}${version === 0 ? "" : ` v${version}`}`);
        const client = this.live();
        const [uri] = client.open(file, version, { text });
        this.documents.set(file, { version, text });
        if (options.pull !== false) {
            // The compile starts on a request that needs it; its answer is
            // not the point.
            void client.pullDiagnostics(uri).catch(() => undefined);
        }
    }

    /// Run `body` as the step `text` against the live server; the server
    /// exiting meanwhile ends the wait.
    private ask<T>(text: string, body: (client: CliceClient) => Promise<T>): Promise<T> {
        return this.steps.run(text, () => {
            const client = this.live();
            const exited = client.exited.then((code) => {
                throw new Error(`the server exited (code ${String(code)})`);
            });
            return Promise.race([body(client), exited]);
        });
    }

    private showLocation(uri: string, range: proto.Range): string {
        const file = this.relative(uri);
        const line = this.text(file).split("\n")[range.start.line] ?? "";
        return `${file}: ${line.trim()}`;
    }

    private killFile(): string {
        return path.join(".clice", "kill-request");
    }
}
