import * as proto from "vscode-languageserver-protocol";
import { markerPoints } from "../annotation.ts";
import {
    enumName,
    fmtRange,
    markerSections,
    OffsetConverter,
    SNAP_ITEM_LIMIT,
    sortedMarkers,
    type Feature,
} from "../render.ts";
import { yamlStr } from "../snapshot.ts";

/// A completion item reduced to the fields the snapshot pins. `edit` is
/// the replace range; `insertRange` the shorter insert range of an
/// insert/replace edit, when it differs. `sortText` orders the entries the
/// way a client does.
export interface CompletionEntry {
    label: string;
    kind: string | null;
    sortText: string;
    filter: string | null;
    detail: string | null;
    description: string | null;
    edit: string | null;
    insertRange: string | null;
    newText: string | null;
    snippet: boolean;
    deprecated: boolean;
}

interface RawPosition {
    line: number;
    character: number;
}

/// Raw inspect JSON for one completion item: the protocol type serialized
/// with native snake_case names and string enums, ranges already in LSP
/// positions.
interface RawRange {
    start: RawPosition;
    end: RawPosition;
}

interface RawCompletionItem {
    label: string;
    kind?: string | null;
    sort_text?: string | null;
    filter_text?: string | null;
    label_details?: { detail?: string | null; description?: string | null } | null;
    tags?: string[] | null;
    insert_text?: string | null;
    insert_text_format?: string | null;
    text_edit?:
        | { range: RawRange; new_text: string }
        | { insert: RawRange; replace: RawRange; new_text: string }
        | null;
}

function renderCompletionEntry(entry: CompletionEntry): string {
    let line = `- { label: ${yamlStr(entry.label)}`;
    if (entry.kind !== null) {
        line += `, kind: ${entry.kind}`;
    }
    if (entry.filter !== null && entry.filter !== entry.label) {
        line += `, filter: ${yamlStr(entry.filter)}`;
    }
    if (entry.detail !== null) {
        line += `, detail: ${yamlStr(entry.detail)}`;
    }
    if (entry.description !== null) {
        line += `, description: ${yamlStr(entry.description)}`;
    }
    if (entry.edit !== null) {
        line += `, edit: "${entry.edit}"`;
    }
    if (entry.insertRange !== null) {
        line += `, insert_range: "${entry.insertRange}"`;
    }
    if (entry.newText !== null && entry.newText !== entry.label) {
        line += `, insert: ${yamlStr(entry.newText)}`;
    }
    if (entry.snippet) {
        line += ", snippet: true";
    }
    if (entry.deprecated) {
        line += ", deprecated: true";
    }
    return line + " }";
}

export function completionLines(entries: CompletionEntry[]): string[] {
    // The feature layer returns candidates unsorted (clang's order is
    // host-dependent), so ties of the sort text fall back to the full
    // rendered line — deterministic on every host, unlike a label sort
    // whose remaining ties kept the host-dependent input order.
    const order = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
    const rendered = entries.map((entry) => ({
        sortText: entry.sortText,
        line: renderCompletionEntry(entry),
    }));
    rendered.sort((a, b) => order(a.sortText, b.sortText) || order(a.line, b.line));
    const out = rendered.slice(0, SNAP_ITEM_LIMIT).map((entry) => entry.line);
    if (rendered.length > SNAP_ITEM_LIMIT) {
        out.push(`… +${rendered.length - SNAP_ITEM_LIMIT} more`);
    }
    return out;
}

function rawRange(range: RawRange): string {
    return (
        `${range.start.line}:${range.start.character}-` + `${range.end.line}:${range.end.character}`
    );
}

function rawCompletionEntry(item: RawCompletionItem): CompletionEntry {
    const edit = item.text_edit ?? null;
    const replace = edit === null ? null : "range" in edit ? edit.range : edit.replace;
    const insert = edit !== null && "insert" in edit ? rawRange(edit.insert) : null;
    return {
        label: item.label,
        kind: item.kind ?? null,
        sortText: item.sort_text ?? "",
        filter: item.filter_text ?? null,
        detail: item.label_details?.detail ?? null,
        description: item.label_details?.description ?? null,
        edit: replace !== null ? rawRange(replace) : null,
        insertRange: insert,
        newText: edit?.new_text ?? item.insert_text ?? null,
        snippet: item.insert_text_format === "Snippet",
        deprecated: item.tags?.includes("Deprecated") ?? false,
    };
}

function replyCompletionEntry(item: proto.CompletionItem): CompletionEntry {
    const edit = item.textEdit;
    const replace = edit === undefined ? null : "range" in edit ? edit.range : edit.replace;
    const insert = edit !== undefined && "insert" in edit ? fmtRange(edit.insert) : null;
    return {
        label: item.label,
        kind: item.kind !== undefined ? enumName(proto.CompletionItemKind, item.kind) : null,
        sortText: item.sortText ?? "",
        filter: item.filterText ?? null,
        detail: item.labelDetails?.detail ?? null,
        description: item.labelDetails?.description ?? null,
        edit: replace !== null ? fmtRange(replace) : null,
        insertRange: insert,
        newText: edit !== undefined ? edit.newText : (item.insertText ?? null),
        snippet: item.insertTextFormat === proto.InsertTextFormat.Snippet,
        deprecated: item.tags?.includes(proto.CompletionItemTag.Deprecated) ?? false,
    };
}

export const codeCompletion: Feature = {
    shape: "completion",
    fromInspect(entry) {
        return markerSections(sortedMarkers(entry.markers ?? {}), (value) =>
            completionLines((value as RawCompletionItem[]).map(rawCompletionEntry)),
        );
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const sections: [string, unknown][] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const pos = map.position(offset);
            const reply = (await client.completionAt(uri, pos.line, pos.character)) ?? [];
            sections.push([name, Array.isArray(reply) ? reply : reply.items]);
        }
        return markerSections(sections, (value) =>
            completionLines((value as proto.CompletionItem[]).map(replyCompletionEntry)),
        );
    },
};
