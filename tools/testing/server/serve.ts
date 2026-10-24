/// The server actions of a case's tests: one server over the case's copy,
/// and everything a scenario does to it.
///
/// Actions are of three kinds. Basic actions send one message, touch the
/// disk once or call one test hook. Combined actions are built from actions
/// only and end on a server event: a reply, the diagnostics of a version, a
/// finished `sync`, a hold or gate reached, a process exit. Nothing sleeps
/// or polls. Every action is recorded as a step; a failing test prints the
/// steps, what the server still had to do, and the log lines that matter.

import * as path from "node:path";
import * as proto from "vscode-languageserver-protocol";
import { URI } from "vscode-uri";
import { CliceClient, type InitializeOptions } from "../../client/client.ts";
import { cliceExecutable } from "../../client/session.ts";
import { Workspace } from "../../client/workspace.ts";
import { anomaliesInLogFiles, serverEnv } from "../../process_gate.ts";
import { withTimeout } from "../../promise.ts";
import {
    WatchRequest,
    type BuildKind,
    type ContextItem,
    type CurrentContextResult,
    type ListConfigurationsResult,
    type LoopEvent,
    type QueryContextResult,
    type StatsResult,
    type SwitchConfigurationResult,
    type SwitchContextResult,
} from "../../protocol/protocol.ts";
import { portable } from "../cli/run.ts";
import { record, recordConfig, recordInvocation, recordToml } from "../coverage/record.ts";
import { failureExcerpt, readLogs, undeclaredErrors } from "../logs/logs.ts";
import {
    place,
    renderDiagnostics,
    type Action,
    type Call,
    type Completion,
    type Counterparts,
    type Diagnostic,
    type Fold,
    type Highlight,
    type Hover,
    type InlayHint,
    type Item,
    type Link,
    type OutlineSymbol,
    type Refused,
    type Renameable,
    type Renamed,
    type Popup,
    type Signatures,
    type Token,
    type WorkspaceSymbol,
} from "../render/answers.ts";
import { Client, type LocationRequest, type ReferenceOptions } from "../render/client.ts";
import { applyEdits, type TextEdit } from "../render/edits.ts";
import { AnswerContext, completion, selection, signatures, tokens } from "../render/lsp.ts";
import { uriToPath } from "../render/paths.ts";
import { withoutBom, type Location, type Range } from "../render/position.ts";
import { shown } from "../render/shown.ts";
import type { Table } from "../render/value.ts";
import {
    applyChange,
    describeAnchor,
    locate,
    spanOf,
    type Anchor,
    type Change,
    type MarkerAnchor,
} from "../cases/anchors.ts";
import { DAMAGE, describeChange, type Disk } from "../cases/disk.ts";
import { FAILED_TEARDOWN_MS, StepRecord } from "./steps.ts";

/// A crash or kill the test provokes in the worker running `request` on
/// `file` ("hover", "compile", "buildPch").
export interface WorkerFault {
    request: string;
    file: string;
    /// A crash the file's text decides: the request crashes where the text
    /// holds `#pragma clang __debug crash` (CLICE_TEST_PRAGMA_CRASH), and a
    /// text without it is served.
    pragma?: true;
    /// A crash of the clang-tidy pass, over a focus file whose text holds
    /// this string (CLICE_TEST_TIDY_CRASH); the compile running it is the
    /// request.
    tidy?: string;
    /// A crash the copy's files provoke on their own, as a damaged cache
    /// blob does: the crash is declared, and a PCH `t.disk.corrupt` damaged
    /// in its middle crashes the compile reading it (CLICE_TEST_DAMAGE).
    input?: true;
}

/// How the test's servers start: `t.start(options)`, and every restart of
/// that server again.
export interface ServerOptions {
    /// initializationOptions over the test defaults, shaped as clice.toml.
    config?: Record<string, unknown>;
    /// The client's capabilities; the most conservative client by default.
    capabilities?: proto.ClientCapabilities;
    /// Workspace folders, workspace-relative; null announces only the root.
    folders?: string[] | null;
    /// Folders the client announces coming and going between its
    /// initialize request and the initialized notification.
    foldersBeforeInitialized?: { added?: string[]; removed?: string[] };
    /// The command line after the executable; `serve` by default.
    args?: string[];
    env?: Record<string, string>;
    /// Every such request crashes its worker as a fault does, attributed to
    /// the request (CLICE_TEST_CRASH_REQUEST); any other crash fails.
    crash?: WorkerFault;
    /// The worker running the first such request dies as a kill from
    /// outside ends it, naming no request (CLICE_TEST_KILL_REQUEST).
    kill?: WorkerFault;
    /// The test misleads the server on purpose: anomalies do not fail it,
    /// and Debug builds do not trap on them. It asserts those it expects.
    anomalies?: true;
    /// The client never reads the server's stderr, as an editor that does
    /// not drain it; `s.client.spawnStderrPump()` starts reading.
    stderr?: "unread";
    /// The test kills workers from outside (`s.killWorkers`): Debug builds
    /// do not trap on the crashes the server reports for them.
    killWorkers?: true;
}

/// Work the server holds where the test parked it (`s.park`).
export interface Park {
    reached(): Promise<void>;
    release(): Promise<void>;
}

/// An event of the server's background loops the test waits for
/// (`s.watch`).
export interface Watch {
    reached(): Promise<void>;
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

interface Document {
    version: number;
    text: string;
}

function methodOf(type: unknown): string {
    if (typeof type === "string") {
        return type;
    }
    return (type as { method: string }).method;
}

/// Record every method a client sends for the coverage check.
function recording(client: CliceClient): CliceClient {
    const request = client.sendRequest.bind(client) as (
        type: unknown,
        ...rest: unknown[]
    ) => Promise<unknown>;
    const notify = client.sendNotification.bind(client) as (
        type: unknown,
        params?: unknown,
    ) => Promise<void>;
    (client as unknown as { sendRequest: typeof request }).sendRequest = (type, ...rest) => {
        record("method", methodOf(type));
        return request(type, ...rest);
    };
    (client as unknown as { sendNotification: typeof notify }).sendNotification = (
        type,
        params,
    ) => {
        record("method", methodOf(type));
        return notify(type, params);
    };
    return client;
}

/// The server's cap on a workspace symbol reply, filled in index iteration
/// order: a reply that long is an arbitrary subset.
const RESULT_CAP = 100;

/// One server of a test over the test's copy; `s` in a scenario.
export class Serve extends Client {
    readonly steps: StepRecord;
    readonly workspace: Workspace;
    private readonly disk: Disk;
    private readonly options: ServerOptions;
    private server: CliceClient | null = null;
    private readonly servers: CliceClient[] = [];
    private readonly documents = new Map<string, Document>();
    /// Where the kill this server may cause is armed, once.
    private readonly killFile = path.join(".clice", `kill-request-${String((serves += 1))}`);
    private killArmed = false;
    /// The workers the test killed from outside (`killWorkers`).
    private killed = 0;
    /// Every window/showMessage the servers sent, oldest first.
    private readonly shownMessages: string[] = [];

    /// Server actions over a copy the caller owns, whose case has `marks`;
    /// nothing starts yet.
    constructor(
        workspace: Workspace,
        options: ServerOptions,
        owner: { marks: readonly MarkerAnchor[]; steps: StepRecord; disk: Disk },
    ) {
        super(owner.marks);
        this.workspace = workspace;
        this.disk = owner.disk;
        this.steps = owner.steps;
        this.options = options;
    }

    /// The faults this server was told to cause, and whether its kill was
    /// taken, for the log gate.
    faults(): Faults {
        const { crash, kill, anomalies } = this.options;
        return {
            ...(crash === undefined ? {} : { crash }),
            ...(kill === undefined
                ? {}
                : { kill: { ...kill, taken: this.workspace.exists(`${this.killFile}.taken`) } }),
            ...(anomalies === undefined ? {} : { anomalies }),
            ...(this.killed === 0 ? {} : { killed: this.killed }),
        };
    }

    private recordToml(file: string): void {
        if (path.basename(file) === "clice.toml" && this.workspace.exists(file)) {
            recordToml(this.workspace.read(file));
        }
    }

    uri(file: string): string {
        return this.workspace.uri(file);
    }

    /// The workspace-relative path of a URI the server sent; an absolute one
    /// for a file outside the workspace.
    relative(uri: string): string {
        const file = URI.parse(uri).fsPath;
        const relative = path.relative(this.workspace.root, file);
        return relative.startsWith("..") ? file : relative.split(path.sep).join("/");
    }

    /// The workspace-relative path a URI the server sent spells, checked
    /// strictly (uriToPath) and kept as spelled: no link resolved, no `.`
    /// or doubled separator dropped. An absolute one outside the workspace.
    spelling(uri: string): string {
        const file = uriToPath(uri);
        const root = URI.file(this.workspace.root).fsPath + path.sep;
        return file.startsWith(root) ? file.slice(root.length).split(path.sep).join("/") : file;
    }

    // Documents.

    /// didOpen with the disk text, or `text` for a buffer the disk does not
    /// hold; then a diagnostics pull a compile starts on, unless `pull` is
    /// false. Nothing waits.
    open(file: string, options: { text?: string; version?: number; pull?: false } = {}): void {
        this.openWith(file, options.text ?? withoutBom(this.workspace.read(file)), options);
    }

    isOpen(file: string): boolean {
        return this.documents.has(file);
    }

    edit(file: string, ...changes: Change[]): void {
        const document = this.document(file);
        let text = document.text;
        for (const change of changes) {
            text = applyChange(text, change, file);
        }
        document.version += 1;
        document.text = text;
        this.steps.note(
            `edit ${file} v${document.version}: ${changes.map(describeChange).join("; ")}`,
        );
        this.live().change(this.uri(file), document.version, text);
    }

    /// Write the buffer to disk and say so (didSave). A file not open, or one
    /// saved with `write: false`, is announced with the disk as it stands.
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

    // Reading results. A request's promise renders through `t.show`.

    /// The diagnostics of `file` as it stands, opening it first if needed:
    /// a pull waits for the compile of the version sent last, and its answer
    /// is what a push carries.
    diagnostics(file: string): Promise<Diagnostic[]> {
        const reply = this.steps.run(`diagnostics ${file}`, async () => {
            if (!this.documents.has(file)) {
                this.open(file, { pull: false });
            }
            const version = this.document(file).version;
            const pulled = await this.ask(`pull ${file} v${version}`, (client) =>
                client.pullDiagnostics(this.uri(file)),
            );
            return this.context().diagnostics(file, pulled);
        });
        return shown(reply, "diagnostics", (diagnostics, { parts }) =>
            this.renderDiagnostics(file, diagnostics, parts),
        );
    }

    /// The errors `file` compiles with.
    errors(file: string): Promise<Diagnostic[]> {
        const reply = this.diagnostics(file).then((diagnostics) =>
            diagnostics.filter((diagnostic) => diagnostic.severity === "error"),
        );
        return shown(reply, "errors", (diagnostics, { parts }) =>
            this.renderDiagnostics(file, diagnostics, parts),
        );
    }

    /// Every diagnostics push `file` received, oldest first, once idle.
    /// It starts no compile.
    pushes(file: string): Promise<Diagnostic[][]> {
        const reply = this.steps.run(`pushes of ${file}`, async () => {
            await this.sync();
            const client = this.live();
            const uri = client.normalizeUri(this.uri(file));
            const context = this.context();
            return client.publishedDiagnostics
                .filter((params) => client.normalizeUri(params.uri) === uri)
                .map((params) => context.diagnostics(context.path(params.uri), params.diagnostics));
        });
        return shown(reply, "pushes", (pushes, { parts }) =>
            pushes.map((push) => this.renderDiagnostics(file, push, parts)),
        );
    }

    /// What the server told the user about the setup ([guidance] messages),
    /// once idle.
    guidance(): Promise<string[]> {
        return this.steps.run("guidance", async () => {
            await this.sync();
            return this.live()
                .guidanceMessages()
                .map((message) =>
                    portable(message.replace(/^\[guidance\] /, ""), this.workspace.root),
                );
        });
    }

    /// How many workers crashed running the crash the server was started
    /// with, or `request` on `file`, once idle.
    crashes(fault: WorkerFault | undefined = this.options.crash): Promise<number> {
        if (fault === undefined) {
            throw new Error("crashes() counts a declared crash: start the server with crash");
        }
        return this.steps.run(`crashes in ${fault.request} ${fault.file}`, async () => {
            await this.sync();
            return this.workspace.workerCrashes(
                `${fault.request} ${this.workspace.displayPath(fault.file)}`,
            );
        });
    }

    /// How many `method` requests the server sent the client, once idle.
    serverRequests(method: string): Promise<number> {
        return this.steps.run(`${method} requests from the server`, async () => {
            await this.sync();
            return this.live().serverRequests.filter((sent) => sent === method).length;
        });
    }

    /// The lines of `file` the server marks inactive, counted from 0.
    inactiveLines(file: string): Promise<number[]> {
        const reply = this.ask(`inactive lines of ${file}`, (client) =>
            client.inactiveLines(this.uri(file)),
        );
        const scope = this.scopeOf(file);
        return shown(reply, "inactive_lines", (lines) => {
            const source = scope.file(file);
            return lines.map((line) =>
                place(
                    {
                        path: file,
                        range: {
                            start: { line, character: 0 },
                            end: { line, character: source.line(line).length },
                        },
                    },
                    scope,
                ),
            );
        });
    }

    private contextItem(item: ContextItem): Table {
        return {
            label: item.label,
            description: portable(item.description, this.workspace.root),
            host: this.relative(item.uri),
        };
    }

    contexts(file: string, options: { offset?: number } = {}): Promise<QueryContextResult> {
        const reply = this.ask(`contexts of ${file}`, (client) =>
            client.queryContext(this.uri(file), options),
        );
        return shown(reply, "contexts", (result) =>
            result.contexts.map((item) => this.contextItem(item)),
        );
    }

    currentContext(file: string): Promise<CurrentContextResult> {
        const reply = this.ask(`current context of ${file}`, (client) =>
            client.currentContext(this.uri(file)),
        );
        return shown(reply, "current_context", (result) => ({
            ...(result.context === null ? {} : { context: this.contextItem(result.context) }),
            automatic: result.automatic,
        }));
    }

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

    /// The configuration menu of the project serving `file`, else of the
    /// first folder's.
    configurations(file?: string): Promise<ListConfigurationsResult> {
        const reply = this.ask(
            `configurations${file === undefined ? "" : ` of ${file}`}`,
            (client) => client.listConfigurations(file === undefined ? undefined : this.uri(file)),
        );
        return shown(reply, "configurations", (result) => ({
            menu: result.configurations,
            active: result.active,
            selected: result.selected,
            default: result.defaultConfiguration,
        }));
    }

    switchConfiguration(name: string, file?: string): Promise<SwitchConfigurationResult> {
        return this.ask(`switch to the configuration ${JSON.stringify(name)}`, (client) =>
            client.switchConfiguration(name, file === undefined ? undefined : this.uri(file)),
        );
    }

    private renderDiagnostics(
        file: string,
        diagnostics: readonly Diagnostic[],
        parts?: ReadonlySet<string>,
    ): Table[] {
        return renderDiagnostics(diagnostics, this.scopeOf(file), parts);
    }

    // Requests: what each one answers is decoded here and rendered by Client.

    protected fetchHover(at: Anchor): Promise<Hover | null> {
        const { uri, position } = this.position(at);
        return this.ask(`hover ${describeAnchor(at)}`, async (client) =>
            this.context().hover(
                at.path,
                await client.hoverAt(uri, position.line, position.character),
            ),
        );
    }

    protected fetchCompletion(at: Anchor, trigger: string | undefined): Promise<Completion> {
        const { uri, position } = this.position(at);
        const by = trigger === undefined ? "" : ` triggered by ${JSON.stringify(trigger)}`;
        return this.ask(`completion ${describeAnchor(at)}${by}`, async (client) =>
            completion(
                await client.completionAt(uri, position.line, position.character, {
                    ...(trigger === undefined ? {} : { triggerCharacter: trigger }),
                }),
                at.path,
            ),
        );
    }

    protected fetchSignatureHelp(at: Anchor, popup: Popup | undefined): Promise<Signatures> {
        const { uri, position } = this.position(at);
        return this.ask(`signatureHelp ${describeAnchor(at)}`, async (client) =>
            signatures(await client.signatureHelpAt(uri, position.line, position.character, popup)),
        );
    }

    protected fetchHighlights(at: Anchor): Promise<Highlight[]> {
        const { uri, position } = this.position(at);
        return this.ask(`highlights ${describeAnchor(at)}`, async (client) => {
            const reply = await client.documentHighlightAt(uri, position.line, position.character);
            if (reply?.some((highlight) => highlight.kind === undefined)) {
                throw new Error("clice always sets the highlight kind");
            }
            return this.context().highlights(at.path, reply);
        });
    }

    protected fetchSelectionRange(at: Anchor): Promise<Range[]> {
        const { uri, position } = this.position(at);
        return this.ask(`selectionRange ${describeAnchor(at)}`, async (client) => {
            const reply = await client.selectionRanges(uri, [position]);
            if (reply?.length !== 1) {
                throw new Error("clice answers every position of a selectionRange request");
            }
            return selection(reply[0]);
        });
    }

    protected fetchCodeActions(
        file: string,
        range: Range,
        only: readonly string[] | undefined,
    ): Promise<Action[]> {
        const kinds = only === undefined ? "" : ` only ${only.join(", ")}`;
        return this.ask(`codeActions ${file} ${JSON.stringify(range)}${kinds}`, async (client) =>
            this.context().actions(await this.actionsOf(client, file, range, only)),
        );
    }

    protected async compileWith(
        file: string,
        edits: ReadonlyMap<string, readonly TextEdit[]>,
    ): Promise<string[]> {
        const context = this.context();
        const originals = new Map([...edits.keys()].map((rel) => [rel, this.text(rel)]));
        const put = (rel: string, text: string): void => {
            if (this.documents.has(rel)) {
                this.edit(rel, { text });
            } else {
                this.workspace.write(rel, text);
            }
        };
        for (const [rel, list] of edits) {
            put(rel, applyEdits(context.file(rel), list));
        }
        const found = (await this.errors(file)).map((error) => error.message).sort();
        for (const [rel, text] of originals) {
            put(rel, text);
        }
        await this.diagnostics(file);
        return found;
    }

    protected fetchLocations(
        request: LocationRequest,
        at: Anchor,
        options: ReferenceOptions = {},
    ): Promise<Location[]> {
        const { uri, position } = this.position(at);
        const { line, character } = position;
        return this.ask(`${request} ${describeAnchor(at)}`, async (client) => {
            switch (request) {
                case "definition":
                    return this.context().locations(
                        await client.definitionAt(uri, line, character),
                    );
                case "declaration":
                    return this.context().locations(
                        await client.declarationAt(uri, line, character),
                    );
                case "type definition":
                    return this.context().locations(
                        await client.typeDefinitionAt(uri, line, character),
                    );
                case "implementation":
                    return this.context().locations(
                        await client.implementationAt(uri, line, character),
                    );
                case "references":
                    return this.context().locations(
                        await client.referencesAt(uri, line, character, {
                            includeDeclaration: options.declarations !== false,
                        }),
                    );
            }
        });
    }

    protected fetchHierarchy(hierarchy: "call" | "type", at: Anchor): Promise<Item[]> {
        const { uri, position } = this.position(at);
        const { line, character } = position;
        return this.ask(`${hierarchy} hierarchy ${describeAnchor(at)}`, async (client) => {
            const items =
                hierarchy === "call"
                    ? await client.prepareCallHierarchy(uri, line, character)
                    : await client.prepareTypeHierarchy(uri, line, character);
            return (items ?? []).map((item) => this.context().item(item));
        });
    }

    protected fetchCalls(direction: "incoming" | "outgoing", at: Anchor): Promise<Call[]> {
        const { uri, position } = this.position(at);
        return this.ask(`${direction} calls ${describeAnchor(at)}`, async (client) => {
            const items = await client.prepareCallHierarchy(uri, position.line, position.character);
            const calls: Call[] = [];
            for (const item of items ?? []) {
                calls.push(
                    ...(direction === "incoming"
                        ? this.context().incoming(await client.callHierarchyIncoming(item))
                        : this.context().outgoing(item, await client.callHierarchyOutgoing(item))),
                );
            }
            return calls;
        });
    }

    protected fetchTypes(direction: "supertypes" | "subtypes", at: Anchor): Promise<Item[]> {
        const { uri, position } = this.position(at);
        return this.ask(`${direction} ${describeAnchor(at)}`, async (client) => {
            const items = await client.prepareTypeHierarchy(uri, position.line, position.character);
            const related: Item[] = [];
            for (const item of items ?? []) {
                const reply =
                    direction === "supertypes"
                        ? await client.typeHierarchySupertypes(item)
                        : await client.typeHierarchySubtypes(item);
                related.push(...(reply ?? []).map((entry) => this.context().item(entry)));
            }
            return related;
        });
    }

    protected fetchTokens(file: string): Promise<Token[]> {
        return this.ask(`semantic tokens of ${file}`, async (client) => {
            const provider = client.initResult?.capabilities.semanticTokensProvider as
                | proto.SemanticTokensOptions
                | undefined;
            if (provider === undefined) {
                throw new Error("the server advertises no semantic tokens legend");
            }
            return tokens(await client.semanticTokensFull(this.uri(file)), provider.legend);
        });
    }

    protected fetchInlayHints(file: string, range: Range): Promise<InlayHint[]> {
        return this.ask(`inlay hints of ${file} ${JSON.stringify(range)}`, async (client) =>
            this.context().inlayHints(await client.inlayHints(this.uri(file), range)),
        );
    }

    protected fetchFolds(file: string): Promise<Fold[]> {
        const uri = this.uri(file);
        return this.ask(`folding ranges of ${file}`, async (client) =>
            this.context().folds(file, await client.foldingRanges(uri)),
        );
    }

    protected fetchSymbols(file: string): Promise<OutlineSymbol[]> {
        const uri = this.uri(file);
        return this.ask(`document symbols of ${file}`, async (client) => {
            return this.context().outline(file, await client.documentSymbols(uri));
        });
    }

    protected fetchLinks(file: string): Promise<Link[]> {
        const uri = this.uri(file);
        return this.ask(`document links of ${file}`, async (client) => {
            const reply = await client.documentLinks(uri);
            const context = this.context();
            // Editors show a tooltip beside the hover card, which already
            // names the target.
            for (const link of reply ?? []) {
                if (link.target === undefined || link.tooltip !== undefined) {
                    throw new Error(
                        `a link at ${JSON.stringify(link.range)} lacks a target or has a tooltip`,
                    );
                }
            }
            return context.links(file, reply);
        });
    }

    protected fetchCounterparts(file: string): Promise<Counterparts> {
        return this.ask(`counterparts of ${file}`, async (client) => {
            const found = this.context().counterparts(await client.counterparts(this.uri(file)));
            if (
                found.preferred !== undefined &&
                !found.candidates.some(({ path: candidate }) => candidate === found.preferred)
            ) {
                throw new Error(`${file}: the preferred counterpart is no candidate`);
            }
            return found;
        });
    }

    protected fetchWorkspaceSymbols(query: string): Promise<WorkspaceSymbol[]> {
        return this.ask(`workspaceSymbols ${JSON.stringify(query)}`, async (client) => {
            const symbols = this.context().workspaceSymbols(await client.workspaceSymbols(query));
            if (symbols.length >= RESULT_CAP) {
                throw new Error(`query '${query}' hits the server's result cap; narrow the case`);
            }
            return symbols;
        });
    }

    protected fetchFormatting(file: string, range: Range | undefined): Promise<TextEdit[] | null> {
        const uri = this.uri(file);
        if (range === undefined) {
            return this.ask(`formatting ${file}`, (client) => client.formatDocument(uri));
        }
        return this.ask(`formatting ${file} ${JSON.stringify(range)}`, (client) =>
            client.formatRange(uri, range),
        );
    }

    protected fetchPrepareRename(at: Anchor): Promise<Renameable | Refused | null> {
        const { uri, position } = this.position(at);
        return this.ask(`prepareRename ${describeAnchor(at)}`, (client) =>
            refusable(async () => {
                const reply = await client.prepareRenameAt(uri, position.line, position.character);
                if (reply === null) {
                    return null;
                }
                if (!("placeholder" in reply)) {
                    throw new Error("clice answers prepareRename with a range and a placeholder");
                }
                return {
                    location: { path: at.path, range: reply.range },
                    placeholder: reply.placeholder,
                };
            }),
        );
    }

    /// The rename's edits, and the messages the server showed while it
    /// answered: it shows its notice before it replies.
    protected fetchRename(at: Anchor, newName: string): Promise<Renamed | Refused | null> {
        const { uri, position } = this.position(at);
        return this.ask(`rename ${describeAnchor(at)} to ${newName}`, (client) =>
            refusable(async () => {
                const shown = this.shownMessages.length;
                const reply = await client.renameAt(
                    uri,
                    position.line,
                    position.character,
                    newName,
                );
                if (reply === null) {
                    return null;
                }
                const context = this.context();
                const versions = new Map<string, number | null>();
                for (const change of reply.documentChanges ?? []) {
                    if ("textDocument" in change) {
                        versions.set(
                            context.path(change.textDocument.uri),
                            change.textDocument.version,
                        );
                    }
                }
                const notices = this.shownMessages.slice(shown);
                return {
                    edits: context.workspaceEdit(reply),
                    versions,
                    ...(notices.length === 0 ? {} : { notice: notices.join("\n") }),
                };
            }),
        );
    }

    private actionsOf(
        client: CliceClient,
        file: string,
        range: Range,
        only: readonly string[] | undefined,
    ): Promise<(proto.Command | proto.CodeAction)[] | null> {
        return client.sendRequest(proto.CodeActionRequest.type, {
            textDocument: { uri: this.uri(file) },
            range,
            context: { diagnostics: [], ...(only === undefined ? {} : { only: [...only] }) },
        });
    }

    /// Any request about a document, at a position for an anchor; `extra`
    /// joins the parameters.
    request<T = unknown>(method: string, where: string | Anchor, extra: object = {}): Promise<T> {
        return this.send<T>(method, where, extra).reply;
    }

    /// A request with the means to cancel it ($/cancelRequest).
    send<T = unknown>(
        method: string,
        where: string | Anchor,
        extra: object = {},
    ): { reply: Promise<T>; cancel(): void } {
        let target: object;
        if (typeof where === "string") {
            target = { textDocument: { uri: this.uri(where) } };
        } else {
            const { uri, position } = this.position(where);
            target = { textDocument: { uri }, position };
        }
        const name = typeof where === "string" ? where : describeAnchor(where);
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

    /// The LSP position of an anchor in the text requests about its file see.
    position(anchor: Anchor): { uri: string; position: proto.Position } {
        const text = this.text(anchor.path);
        const offset = locate(text, anchor);
        return {
            uri: this.uri(anchor.path),
            position: this.context().file(anchor.path).positionAt(offset),
        };
    }

    /// The range an anchor's snippet covers.
    range(anchor: Anchor): proto.Range {
        const text = this.text(anchor.path);
        const { begin, end } = spanOf(text, anchor);
        const source = this.context().file(anchor.path);
        return { start: source.positionAt(begin), end: source.positionAt(end) };
    }

    // Test hooks.

    /// Wait until the server has no work left (clice/internal/sync), after a
    /// look at the disk with `poll`; work still pending at its deadline
    /// fails the step with that work.
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

    stats(): Promise<StatsResult> {
        return this.ask("stats", (client) => client.stats());
    }

    /// Look at the disk once (clice/internal/poll); returns the file events
    /// the look produced. A database look reloads at once unless `force` is
    /// false, which keeps the background loop's stamp check and debounce.
    poll(loop: "cdb" | "workspace", options: { force?: boolean } = {}): Promise<number> {
        return this.ask(
            `poll ${loop}${options.force === false ? " by its stamp" : ""}`,
            async (client) => (await client.poll(loop, options)).events,
        );
    }

    /// Park the next `work` on `file` until the test releases it: at its
    /// reply, for a build ("compile", "pch", "pcm", "index"; held in the
    /// master as clice/internal/hold), or where it starts in its worker
    /// ("compile", "completion", "tuRun", "hover"; gated as
    /// clice/internal/gate), so a cancel, an edit or a death arrives mid-work.
    /// Resolves once placed; a park left unreleased goes with the server.
    async park(work: string, file: string, options: { at: "reply" | "worker" }): Promise<Park> {
        const where =
            options.at === "reply"
                ? `the ${work} reply of ${file}`
                : `the ${work} of ${file} in its worker`;
        const id = await this.ask(`park ${where}`, (client) =>
            options.at === "reply"
                ? client.hold(work as BuildKind, this.uri(file))
                : client.gate(work, this.uri(file)),
        );
        // A park is its server's: one stopped, killed or restarted took its
        // parks with it, and the next numbers its own from 1 again.
        const placed = this.server;
        return {
            reached: () =>
                this.server === placed
                    ? this.ask(`${where} parks`, (client) => client.parkedBy(id))
                    : Promise.reject(new Error(`${where}: its server is gone`)),
            release: () =>
                this.server === placed
                    ? this.ask(`release ${where}`, (client) => client.release(id))
                    : Promise.resolve(),
        };
    }

    /// Watch for the next `event` of the server's background loops, which
    /// no request of the test's starts (clice/internal/watch): a database
    /// change the database loop applied, files the disk loop found
    /// changed, a checkpoint that committed index shards. Resolves once
    /// placed; `reached` resolves at the event.
    async watch(event: LoopEvent): Promise<Watch> {
        const id = await this.ask(
            `watch for ${event}`,
            async (client) => (await client.sendRequest(WatchRequest, { event })).id,
        );
        const placed = this.server;
        return {
            reached: () =>
                this.server === placed
                    ? this.ask(`${event} seen`, (client) => client.parkedBy(id))
                    : Promise.reject(new Error(`the ${event} watch: its server is gone`)),
        };
    }

    // The server.

    /// Start a server on the workspace, its cache the one a server before it
    /// left, with the options the test gave.
    start(): Promise<void> {
        return this.steps.run("start the server", async () => {
            this.launch();
            await this.initialize();
        });
    }

    /// The handshake, initialize and initialized, with the test's options:
    /// part of a start, or what a test sends a spawned server.
    initialize(): Promise<void> {
        return this.steps.run("initialize", async () => {
            const client = this.live();
            this.recordToml("clice.toml");
            const { config, capabilities, folders, foldersBeforeInitialized } = this.options;
            if (config !== undefined) {
                recordConfig(config);
            }
            const initialize: InitializeOptions = {
                initializationOptions: config,
                capabilities,
                folders,
                ...(foldersBeforeInitialized === undefined
                    ? {}
                    : {
                          beforeInitialized: () =>
                              client.changeWorkspaceFolders(foldersBeforeInitialized),
                      }),
            };
            await client.initialize(this.workspace, initialize);
        });
    }

    /// Start a server that waits for the client to speak first: a replay
    /// sends the initialize it recorded, a test `initialize()` or its own.
    spawn(): Promise<void> {
        return this.steps.run("spawn the server", () => {
            this.launch();
            return Promise.resolve();
        });
    }

    private launch(): CliceClient {
        const env: Record<string, string> = { ...this.options.env };
        if (
            this.options.anomalies === true ||
            this.options.crash !== undefined ||
            this.options.killWorkers === true
        ) {
            env["CLICE_ANOMALY_NO_TRAP"] = "1";
        }
        const { kill, crash } = this.options;
        if (kill !== undefined) {
            if (!this.killArmed) {
                this.killArmed = true;
                this.workspace.write(
                    this.killFile,
                    `${kill.request} ${this.workspace.displayPath(kill.file)}`,
                );
            }
            env["CLICE_ANOMALY_NO_TRAP"] = "1";
            env["CLICE_TEST_KILL_REQUEST"] = this.workspace.path(this.killFile);
        }
        if (crash?.tidy !== undefined) {
            env["CLICE_TEST_TIDY_CRASH"] = crash.tidy;
        } else if (crash?.pragma === true) {
            env["CLICE_TEST_PRAGMA_CRASH"] = "1";
        } else if (crash?.input === true) {
            env["CLICE_TEST_DAMAGE"] = DAMAGE;
        } else if (crash !== undefined) {
            env["CLICE_TEST_CRASH_REQUEST"] =
                `${crash.request} ${this.workspace.displayPath(crash.file)}`;
        }
        const args = this.options.args;
        recordInvocation(args ?? ["serve"]);
        const client = recording(
            CliceClient.start(cliceExecutable(), {
                args,
                env: { ...(serverEnv() as Record<string, string>), ...env },
                drainStderr: this.options.stderr !== "unread",
            }),
        );
        client.onNotification(proto.ShowMessageNotification.type, (params) => {
            this.shownMessages.push(params.message);
        });
        this.server = client;
        this.servers.push(client);
        return client;
    }

    /// Shut the server down through its exit gate; its documents go with it.
    stop(): Promise<void> {
        return this.steps.run("stop the server", async () => {
            await this.live().shutdown();
            this.server = null;
            this.documents.clear();
        });
    }

    /// Kill every worker of `kind` in service from outside, as the OOM
    /// killer or a user would: the death names no request. Placed after a
    /// park is reached, it lands mid-work. Returns how many.
    killWorkers(kind: "stateful" | "stateless"): Promise<number> {
        if (this.options.killWorkers !== true) {
            throw new Error("killWorkers() needs the server started with killWorkers: true");
        }
        return this.ask(`kill the ${kind} workers`, async (client) => {
            const pids = (await client.stats()).workers[kind];
            for (const pid of pids) {
                process.kill(pid, "SIGKILL");
            }
            this.killed += pids.length;
            return pids.length;
        });
    }

    /// Kill the server from outside and wait for it to be gone.
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

    /// Stop and start the server, the cache kept, and open again the
    /// documents it had open with their buffers.
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

    /// Run `body` with the server down and start one again, failing or not.
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

    /// Stop the server, remove its cache (the logs stay), start one again.
    cold(): Promise<void> {
        return this.steps.run("cold start", async () => {
            await this.stop();
            this.workspace.rm(".clice/cache");
            await this.start();
        });
    }

    /// Announce workspace folders coming and going, workspace-relative.
    changeFolders(change: { added?: string[]; removed?: string[] }): Promise<void> {
        const parts = [
            ...(change.added ?? []).map((f) => `+${f}`),
            ...(change.removed ?? []).map((f) => `-${f}`),
        ];
        return this.ask(`folders ${parts.join(" ")}`, (client) =>
            client.changeWorkspaceFolders(change),
        );
    }

    // Combined actions.

    /// The server went idle with every unit indexed.
    indexed(): Promise<void> {
        return this.steps.run("indexed", async () => {
            const { failed } = await this.sync();
            if (failed.length > 0) {
                throw new Error(`units failed to index: ${failed.join(", ")}`);
            }
        });
    }

    /// Apply a workspace edit as an editor does: to the buffer of an open
    /// file, to the disk of another. An edit for another version of an open
    /// buffer is refused, as editors refuse it.
    apply(edit: proto.WorkspaceEdit): Promise<void> {
        return this.steps.run("apply a workspace edit", () => {
            for (const change of edit.documentChanges ?? []) {
                if ("textDocument" in change) {
                    const file = this.relative(change.textDocument.uri);
                    const { version } = change.textDocument;
                    if (version !== null && version !== this.documents.get(file)?.version) {
                        throw new Error(
                            `the edit is for ${file} v${version}, which is not the open buffer`,
                        );
                    }
                }
            }
            const context = this.context();
            for (const [file, edits] of context.workspaceEdit(edit)) {
                const text = applyEdits(context.file(file), edits);
                if (this.documents.has(file)) {
                    this.edit(file, { text });
                } else {
                    this.disk.write(file, text);
                }
            }
            return Promise.resolve();
        });
    }

    /// Apply the code action titled `title` at `anchor`, the cursor of a
    /// point or a range marker's range; returns the diagnostics the file
    /// compiles with afterwards.
    applyAction(anchor: Anchor, title: string): Promise<Diagnostic[]> {
        const reply = this.steps.run(
            `apply ${JSON.stringify(title)} at ${describeAnchor(anchor)}`,
            async () => {
                const { position } = this.position(anchor);
                const range =
                    "name" in anchor && anchor.range
                        ? this.range(anchor)
                        : { start: position, end: position };
                const actions =
                    (await this.ask(`codeActions ${describeAnchor(anchor)}`, (client) =>
                        this.actionsOf(client, anchor.path, range, undefined),
                    )) ?? [];
                const action = actions.find(
                    (candidate): candidate is proto.CodeAction =>
                        candidate.title === title && "edit" in candidate,
                );
                if (action?.edit === undefined) {
                    const offered = actions.map((a) => JSON.stringify(a.title)).join(", ");
                    throw new Error(
                        `no action ${JSON.stringify(title)} with an edit; offered: ${offered}`,
                    );
                }
                await this.apply(action.edit);
                return this.diagnostics(anchor.path);
            },
        );
        return shown(reply, "diagnostics", (diagnostics, { parts }) =>
            this.renderDiagnostics(anchor.path, diagnostics, parts),
        );
    }

    // Teardown.

    /// Shut every server down through its exit gate; a failed test first
    /// hears what the server still had to do. Returns the failures, for the
    /// copy's owner to gate with its logs.
    async finish(failed: boolean): Promise<unknown[]> {
        const errors: unknown[] = [];
        const client = this.server;
        if (failed) {
            if (client !== null && !client.disposed) {
                try {
                    const { pending } = await withTimeout(
                        client.sync({ deadlineMs: 1_000 }),
                        FAILED_TEARDOWN_MS,
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
        for (const server of this.servers.reverse()) {
            try {
                if (!server.disposed) {
                    await server.shutdown(
                        failed ? { verbose: true, bound: FAILED_TEARDOWN_MS } : {},
                    );
                }
            } catch (error) {
                errors.push(error);
            }
        }
        return errors;
    }

    /// The running server's client: for raw protocol the actions do not
    /// send. A test using it says why in a comment.
    get client(): CliceClient {
        return this.live();
    }

    protected context(): AnswerContext {
        return new AnswerContext(this.workspace.root, (file) => this.text(file));
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
        return this.documents.get(file)?.text ?? withoutBom(this.workspace.read(file));
    }

    private openWith(
        file: string,
        text: string,
        options: { version?: number; pull?: false } = {},
    ): void {
        const version = options.version ?? 0;
        this.steps.note(`open ${file}${version === 0 ? "" : ` v${version}`}`);
        const client = this.live();
        // Absolute: a spawned server's client has no workspace bound before
        // its handshake.
        const [uri] = client.open(this.workspace.path(file), version, { text });
        this.documents.set(file, { version, text });
        if (options.pull !== false) {
            // The pull starts the compile; its answer is not the point.
            void client.pullDiagnostics(uri).catch(() => undefined);
        }
    }

    /// Run `body` as a step against the live server; the server exiting
    /// meanwhile ends the wait.
    private ask<T>(text: string, body: (client: CliceClient) => Promise<T>): Promise<T> {
        return this.steps.run(text, () => {
            const client = this.live();
            const exited = client.exited.then((code) => {
                throw new Error(`the server exited (code ${String(code)})`);
            });
            // A closed connection throws at once; the race still observes
            // the exit's rejection.
            let reply: Promise<T>;
            try {
                reply = body(client);
            } catch (error) {
                reply = Promise.reject(error instanceof Error ? error : new Error(String(error)));
            }
            return Promise.race([reply, exited]);
        });
    }
}

/// The error a request is refused with; a reply that is no refusal fails
/// the test.
export async function refusal(reply: Promise<unknown>): Promise<{ code: number; message: string }> {
    const error = await reply.then(
        () => null,
        (failure: unknown) => failure,
    );
    if (!(error instanceof proto.ResponseError)) {
        throw new Error(`expected an error reply, got ${JSON.stringify(error)}`);
    }
    return { code: error.code, message: error.message };
}

let serves = 0;

/// A request whose refusal is an answer: the error the server replies with
/// becomes the message an editor shows.
async function refusable<T>(body: () => Promise<T>): Promise<T | Refused> {
    try {
        return await body();
    } catch (error) {
        if (error instanceof proto.ResponseError) {
            return { error: error.message };
        }
        throw error;
    }
}

/// What a server of the test was told to cause.
export interface Faults {
    crash?: WorkerFault;
    kill?: WorkerFault & { taken: boolean };
    anomalies?: true;
    /// Workers the test killed from outside.
    killed?: number;
}

/// What a test declared about the copy's logs: the error lines it provokes
/// and the faults of each server it started.
export interface Declared {
    errors?: readonly RegExp[];
    servers?: readonly Faults[];
    /// The crashes the test's batch commands provoke, `t.cli(…, { crash })`.
    commands?: readonly Faults[];
}

export const CRASHED_IN = /\] clice worker crashed in: clice\/worker\/(\S+) (.*)$/;

/// The logs a worker's crash is told in: a server's, and a batch command's
/// that runs workers of its own.
const SUPERVISOR_LOGS = new Set(["master.log", "index.log", "lint.log"]);

/// The error-level log lines, crashes, kills and anomalies a copy's servers
/// left, against what the test declared: each declared fault happened, and
/// nothing else did. On failure, the servers' warnings and errors.
export function gateLogs(workspace: Workspace, declared: Declared, failed: boolean): unknown[] {
    const errors: unknown[] = [];
    const servers = [...(declared.servers ?? []), ...(declared.commands ?? [])];
    const crashes = servers.flatMap(({ crash }) => (crash === undefined ? [] : [crash]));
    const kills = servers.flatMap(({ kill }) => (kill === undefined ? [] : [kill]));
    const killed = servers.reduce((sum, server) => sum + (server.killed ?? 0), 0);
    const expected = [...(declared.errors ?? [])];
    if (crashes.length + kills.length + killed > 0) {
        expected.push(
            /\[anomaly:WorkerCrash\] |Worker \S+ lost|Last (stderr|output) of crashed worker/,
        );
    }
    const logs = readLogs(workspace.root);
    const undeclared = undeclaredErrors(logs, expected);
    if (undeclared.length > 0) {
        errors.push(
            new Error(
                `error-level log lines the test did not declare:\n${undeclared.map((l) => `${l.file}: ${l.text}`).join("\n")}`,
            ),
        );
    }
    const crashedIn = logs
        .filter((line) => SUPERVISOR_LOGS.has(line.file))
        .flatMap((line) => {
            const match = CRASHED_IN.exec(line.text);
            return match === null ? [] : [`${match[1] ?? ""} ${match[2] ?? ""}`];
        });
    const named = crashes.map((crash) => `${crash.request} ${workspace.displayPath(crash.file)}`);
    const stray = crashedIn.filter((crash) => !named.includes(crash));
    if (stray.length > 0) {
        errors.push(new Error(`worker crashes nothing declared:\n${stray.join("\n")}`));
    }
    const missed = named.filter((crash) => !crashedIn.includes(crash));
    if (missed.length > 0) {
        errors.push(new Error(`declared crashes that never happened:\n${missed.join("\n")}`));
    }
    const untaken = kills.filter((kill) => !kill.taken);
    if (untaken.length > 0) {
        errors.push(
            new Error(
                `declared kills no worker ran into:\n${untaken.map((kill) => `${kill.request} ${kill.file}`).join("\n")}`,
            ),
        );
    }
    if (!servers.some(({ anomalies }) => anomalies === true)) {
        const anomalies = anomaliesInLogFiles(workspace.root);
        const explained = crashedIn.length + kills.length - untaken.length + killed;
        const crashed = anomalies.filter((anomaly) => anomaly.startsWith("WorkerCrash "));
        if (crashed.length !== anomalies.length || crashed.length !== explained) {
            errors.push(
                new Error(
                    `the declared faults explain ${String(explained)} worker crashes and no other anomaly; the logs have:\n${anomalies.join("\n") || "(none)"}`,
                ),
            );
        }
    }
    if (failed || errors.length > 0) {
        console.log(`server warnings and errors:\n${failureExcerpt(logs)}`);
    }
    return errors;
}
