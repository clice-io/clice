import type * as proto from "vscode-languageserver-protocol";
import { SETTLE_TIME, sleep } from "../../client/client.ts";
import { markerPoints, markerRanges } from "../annotation.ts";
import {
    directiveLines,
    fmtPos,
    fmtRange,
    markerSections,
    OffsetConverter,
    sortedMarkers,
    type Feature,
} from "../render.ts";
import { normalizeFileUri, yamlStr } from "../snapshot.ts";

interface EditEntry {
    /// `${WS}`-relative path of a file other than the requested one.
    file?: string | undefined;
    range: string;
    text: string;
}

interface ActionEntry {
    title: string;
    kind: string;
    edits: EditEntry[];
    /// The inspect path's unresolved host-source request: the definitions
    /// the host would receive.
    hostDefinitions: string[];
}

interface RawTextReplacement {
    range: { begin: number; end: number };
    text: string;
}

interface RawCodeAction {
    title: string;
    kind: string;
    edits: RawTextReplacement[];
    host_definitions?: string[] | null;
}

/// One block per action:
///
///     - [refactor.rewrite] Define 'S::f' out of line
///       3:0-3:0 => "\nvoid S::f() {\n}\n"
function formatActions(actions: ActionEntry[]): string[] {
    if (actions.length === 0) {
        return ["(no actions)"];
    }
    const out: string[] = [];
    for (const action of actions) {
        out.push(`- [${action.kind}] ${action.title}`);
        for (const edit of action.edits) {
            const file = edit.file === undefined ? "" : `${edit.file} `;
            out.push(`  ${file}${edit.range} => ${yamlStr(edit.text)}`);
        }
        if (action.hostDefinitions.length > 0) {
            out.push("  into the host source:");
            out.push(...action.hostDefinitions.map((piece) => `    ${yamlStr(piece)}`));
        }
    }
    return out;
}

function adaptRaw(result: unknown, map: OffsetConverter): ActionEntry[] {
    return (result as RawCodeAction[]).map((action) => ({
        title: action.title,
        kind: action.kind,
        edits: action.edits.map((edit) => ({
            range: `${fmtPos(map.position(edit.range.begin))}-${fmtPos(map.position(edit.range.end))}`,
            text: edit.text,
        })),
        hostDefinitions: action.host_definitions ?? [],
    }));
}

function adaptReply(
    reply: (proto.Command | proto.CodeAction)[] | null,
    uri: string,
    root: string,
): ActionEntry[] {
    return (reply ?? []).map((item) => {
        if (!("title" in item) || "command" in item) {
            throw new Error("clice replies with code actions, never commands");
        }
        const action = item;
        if (action.kind === undefined || action.edit?.documentChanges === undefined) {
            throw new Error("clice always replies with a kind and versioned document changes");
        }
        const edits: EditEntry[] = [];
        for (const change of action.edit.documentChanges) {
            if (!("textDocument" in change)) {
                throw new Error("clice never replies with resource operations");
            }
            const file =
                change.textDocument.uri === uri
                    ? undefined
                    : normalizeFileUri(change.textDocument.uri, root);
            for (const edit of change.edits) {
                if (!("newText" in edit)) {
                    throw new Error("clice replies with plain text edits, never snippets");
                }
                edits.push({ file, range: fmtRange(edit.range), text: edit.newText });
            }
        }
        return { title: action.title, kind: action.kind, edits, hostDefinitions: [] };
    });
}

export const codeAction: Feature = {
    shape: "selection",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return markerSections(sortedMarkers(entry.markers ?? {}), (value) =>
            formatActions(adaptRaw(value, map)),
        );
    },
    async fromServer(client, uri, ctx) {
        // The index-backed actions (definitions vetted and placed in the
        // host, headers declaring a name) race the background index: an
        // indexing fixture names the symbols that must have arrived from
        // another file's rows — the requested file's own session already
        // knows the declarations it sees, which is not the fact awaited.
        const indexed = directiveLines(ctx.stripped, "indexed");
        if (indexed.length > 0 && ctx.indexing !== true) {
            throw new Error("'// indexed:' lines require 'indexing: true' in the fixture meta");
        }
        for (const name of indexed) {
            let elsewhere = false;
            for (let i = 0; i < 60 && !elsewhere; i++) {
                const symbols = (await client.workspaceSymbols(name)) ?? [];
                elsewhere = symbols.some(
                    (symbol) => symbol.name === name && symbol.location.uri !== uri,
                );
                if (!elsewhere) {
                    await sleep(SETTLE_TIME);
                }
            }
            if (!elsewhere) {
                throw new Error(`symbol '${name}' never arrived from another file's index rows`);
            }
        }
        const map = new OffsetConverter(ctx.stripped);
        const sections: [string, ActionEntry[]][] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const position = map.position(offset);
            const reply = await client.codeActions(uri, { start: position, end: position });
            sections.push([name, adaptReply(reply, uri, ctx.root)]);
        }
        for (const [name, [begin, end]] of markerRanges(ctx.source)) {
            const reply = await client.codeActions(uri, {
                start: map.position(begin),
                end: map.position(end),
            });
            sections.push([name, adaptReply(reply, uri, ctx.root)]);
        }
        // The inspect path orders markers by name; mirror it.
        const byName = new Map(sections);
        return markerSections(
            sortedMarkers(Object.fromEntries(sections)).map(([name]) => [name, byName.get(name)]),
            (value) => formatActions(value as ActionEntry[]),
        );
    },
};
