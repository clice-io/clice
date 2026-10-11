/// An answers test: its script asks a client about the code as the case
/// wrote it, once on `clice inspect` and once on a real server, each over
/// its own copy, and the two must show the same.

import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import type * as proto from "vscode-languageserver-protocol";
import { Workspace } from "../../client/workspace.ts";
import { withTimeout } from "../../promise.ts";
import { Inspect } from "../inspect/inspect.ts";
import type { Client } from "../render/client.ts";
import { block, type ShowOptions, type Shown } from "../render/shown.ts";
import { plain, type Table } from "../render/value.ts";
import { Serve, gateLogs } from "../server/serve.ts";
import { SCRIPT_DEADLINE_MS, StepRecord, throwFailures } from "../server/steps.ts";
import { at, type MarkerAnchor, type TextAnchor } from "./anchors.ts";
import type { Case } from "./case.ts";
import { Disk } from "./disk.ts";
import { Output, writeSnapshot, type Blocks } from "./output.ts";
import { materialize, writeDatabases } from "./project.ts";
import type { AnswersOptions } from "./script.ts";

/// The requests an answers test asks, the same on both clients.
const REQUESTS = [
    "hover",
    "completion",
    "signatureHelp",
    "highlights",
    "selectionRange",
    "codeActions",
    "definition",
    "declaration",
    "typeDefinition",
    "implementation",
    "references",
    "incomingCalls",
    "outgoingCalls",
    "supertypes",
    "subtypes",
    "semanticTokens",
    "inlayHints",
    "foldingRanges",
    "documentSymbols",
    "documentLinks",
    "counterparts",
    "workspaceSymbols",
] as const;

export type Requests = Pick<Client, (typeof REQUESTS)[number]>;

export interface AnswersContext extends Requests {
    /// A block of the output: what a request answers, passed as the request
    /// itself, `a.show(m.name, a.hover(m))`; text or lines; or a record of
    /// these, one block each.
    show(label: string, value: Shown, options?: ShowOptions): Promise<void>;
    /// The markers of the case's sources, in path and source order.
    markers(): readonly MarkerAnchor[];
    marker(name: string): MarkerAnchor;
    at(file: string, anchor: string): TextAnchor;
}

/// The client the answers render for: versioned edits, snippets,
/// insert-and-replace ranges, inlay hint label parts.
const EDITOR_CAPABILITIES: proto.ClientCapabilities = {
    workspace: { workspaceEdit: { documentChanges: true } },
    textDocument: {
        completion: { completionItem: { snippetSupport: true, insertReplaceSupport: true } },
        inlayHint: {},
        rename: { prepareSupport: true },
    },
};

const HEADER = /\.(h|hh|hpp|hxx)$/;
const SOURCE = /\.(c|cc|cpp|cxx|cppm|ixx)$/;

/// What one client showed, and the same rendered whole: what the two must
/// agree on beyond what shows.
interface Shows {
    output: Output;
    compared: Output;
}

function contextOf(c: Case, client: Client, root: string, shows: Shows): AnswersContext {
    const requests = Object.fromEntries(
        REQUESTS.map((name) => [name, client[name].bind(client)]),
    ) as unknown as Requests;
    return {
        ...requests,
        async show(label, value, options = {}) {
            const parts = new Set(options.parts);
            shows.output.block(label, await block(value, { parts, full: false }, root, label));
            shows.compared.block(label, await block(value, { parts, full: true }, root, label));
        },
        markers: () => c.markers,
        marker: (name) => markerOf(c, name),
        at,
    };
}

export function markerOf(c: Case, name: string): MarkerAnchor {
    const found = c.markers.find((marker) => marker.name === name);
    if (found === undefined) {
        throw new Error(`${c.id}: no marker §(${name})`);
    }
    return found;
}

function gateCompile(c: Case, client: string, errors: readonly string[]): void {
    if (errors.length > 0 && c.project.compiles !== false) {
        throw new Error(
            `${c.id}: the code does not compile cleanly on ${client}:\n  ${errors.join("\n  ")}`,
        );
    }
    if (errors.length === 0 && c.project.compiles === false) {
        throw new Error(
            `${c.id}: project.jsonc says compiles: false, but the code compiles cleanly on ${client}`,
        );
    }
}

/// A fresh copy of the case, its databases written.
function copy(c: Case, prefix: string): { root: string; copied: string[] } {
    const root = fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), prefix)));
    const copied = materialize(c.dir, root);
    writeDatabases(c.project, copied, root);
    return { root, copied };
}

function remove(root: string): void {
    if (process.env["CLICE_KEEP_WORKSPACE"] === undefined) {
        fs.rmSync(root, { recursive: true, force: true });
    } else {
        console.log(`workspace kept at ${root}`);
    }
}

async function onInspect(
    c: Case,
    options: AnswersOptions,
    fn: (a: AnswersContext) => Promise<void>,
): Promise<Shows> {
    const { root, copied } = copy(c, "clice-inspect-");
    const client = new Inspect(root, c.markers, {
        sources: copied.filter((file) => SOURCE.test(file)),
        config: options.config,
    });
    const shows: Shows = { output: new Output(), compared: new Output() };
    const run = async (): Promise<void> => {
        await fn(contextOf(c, client, root, shows));
        gateCompile(c, "clice inspect", await client.compileErrors());
    };
    try {
        await withTimeout(run(), SCRIPT_DEADLINE_MS, "the test hung");
        return shows;
    } finally {
        client.close();
        remove(root);
    }
}

/// The server opens every source, module interfaces first and the entry
/// last, then every marked header, before the script asks.
function opened(c: Case, copied: readonly string[]): string[] {
    const rank = (file: string): number =>
        /^main\.[a-z]+$/.test(file) ? 2 : file.endsWith(".cppm") ? 0 : 1;
    const sources = copied
        .filter((file) => SOURCE.test(file))
        .sort((a, b) => rank(a) - rank(b) || (a < b ? -1 : 1));
    const headers = [...new Set(c.markers.map((marker) => marker.path))]
        .filter((file) => HEADER.test(file))
        .sort();
    return [...sources, ...headers];
}

async function onServer(
    c: Case,
    options: AnswersOptions,
    fn: (a: AnswersContext) => Promise<void>,
): Promise<Shows> {
    const { root, copied } = copy(c, "clice-answers-");
    const workspace = new Workspace(root);
    const steps = new StepRecord();
    const config = options.config ?? {};
    const project = (config["project"] ?? {}) as Record<string, unknown>;
    const s = new Serve(
        workspace,
        {
            capabilities: EDITOR_CAPABILITIES,
            config: {
                ...config,
                project: { ...project, enable_indexing: options.indexing === true },
            },
        },
        { marks: c.markers, steps, disk: new Disk(root, c.project, steps) },
    );
    const shows: Shows = { output: new Output(), compared: new Output() };
    const run = async (): Promise<void> => {
        await s.start();
        const errors: string[] = [];
        for (const file of opened(c, copied)) {
            errors.push(...(await s.errors(file)).map((error) => error.message));
        }
        gateCompile(c, "the server", errors);
        if (options.indexing === true) {
            await s.indexed();
        }
        await fn(contextOf(c, s, root, shows));
        await s.stop();
    };
    let failure: { error: unknown } | undefined;
    try {
        await withTimeout(run(), SCRIPT_DEADLINE_MS, "the test hung");
    } catch (error) {
        failure = { error };
        console.log(`steps:\n${steps.render()}`);
    }
    const failed = failure !== undefined;
    const gated = [...(await s.finish(failed)), ...gateLogs(workspace, {}, failed)];
    remove(root);
    throwFailures(failure, gated);
    return shows;
}

/// The blocks of each client under its name, for paths pinned apart: a
/// known-wrong reason then goes on the client that answers wrong.
function apart(inspect: Blocks, server: Blocks): Blocks {
    return new Map<string, Table>([
        ["inspect", Object.fromEntries(inspect)],
        ["server", Object.fromEntries(server)],
    ]);
}

/// Run an answers test on the clients it names; returns its blocks.
export async function runAnswers(
    c: Case,
    options: AnswersOptions,
    fn: (a: AnswersContext) => Promise<void>,
): Promise<Blocks> {
    const only = options.paths;
    const runs: Promise<{ client: string; shows: Shows }>[] = [];
    if (only === undefined || !("server" in only)) {
        runs.push(onInspect(c, options, fn).then((shows) => ({ client: "inspect", shows })));
    }
    if (only === undefined || !("inspect" in only)) {
        runs.push(onServer(c, options, fn).then((shows) => ({ client: "server", shows })));
    }
    // Both run to the end, so neither outlives the test.
    const settled = await Promise.allSettled(runs);
    const rejected = settled.find((result) => result.status === "rejected");
    if (rejected !== undefined) {
        throw rejected.reason;
    }
    const [first, second] = settled.flatMap((result) =>
        result.status === "fulfilled" ? [result.value] : [],
    );
    if (first === undefined) {
        throw new Error(`${c.id}: the test runs on no client`);
    }
    const whole = ({ shows }: { shows: Shows }): string =>
        JSON.stringify([...shows.compared.blocks].map(([label, table]) => [label, plain(table)]));
    const separate = only !== undefined && "separate" in only;
    if (second !== undefined && separate && whole(first) === whole(second)) {
        throw new Error(`${c.id}: inspect and server agree; the paths are not separate`);
    }
    if (second === undefined || whole(first) === whole(second)) {
        return first.shows.output.blocks;
    }
    if (separate) {
        return apart(first.shows.output.blocks, second.shows.output.blocks);
    }
    const shown = ({ client, shows }: { client: string; shows: Shows }): string =>
        writeSnapshot(new Map([[client, shows.compared.blocks]]));
    throw new Error(
        `${c.id}: inspect and server answer differently\n${shown(first)}\n${shown(second)}`,
    );
}
