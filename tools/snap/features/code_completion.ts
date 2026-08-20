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

/// A completion item reduced to the fields the snapshot pins. `score` is
/// the fuzzy match score the feature layer stores in sort_text.
export interface CompletionEntry {
    label: string;
    kind: string | null;
    score: number;
    detail: string | null;
    description: string | null;
    edit: string | null;
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
interface RawCompletionItem {
    label: string;
    kind?: string | null;
    sort_text?: string | null;
    label_details?: { detail?: string | null; description?: string | null } | null;
    tags?: string[] | null;
    insert_text?: string | null;
    insert_text_format?: string | null;
    text_edit?: { range: { start: RawPosition; end: RawPosition }; new_text: string } | null;
}

function renderCompletionEntry(entry: CompletionEntry): string {
    let line = `- { label: ${yamlStr(entry.label)}`;
    if (entry.kind !== null) {
        line += `, kind: ${entry.kind}`;
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
    // host-dependent) and scores carry architecture-dependent last bits
    // (FMA contraction), so the snapshot order quantizes the score and
    // then falls back to the full rendered line — deterministic on every
    // host, unlike a raw score-then-label sort whose remaining ties kept
    // the host-dependent input order.
    const rendered = entries.map((entry) => ({
        rank: Math.round(entry.score * 1e4),
        line: renderCompletionEntry(entry),
    }));
    rendered.sort((a, b) => b.rank - a.rank || (a.line < b.line ? -1 : a.line > b.line ? 1 : 0));
    const out = rendered.slice(0, SNAP_ITEM_LIMIT).map((entry) => entry.line);
    if (rendered.length > SNAP_ITEM_LIMIT) {
        out.push(`… +${rendered.length - SNAP_ITEM_LIMIT} more`);
    }
    return out;
}

function rawRange(range: { start: RawPosition; end: RawPosition }): string {
    return (
        `${range.start.line}:${range.start.character}-` + `${range.end.line}:${range.end.character}`
    );
}

function rawCompletionEntry(item: RawCompletionItem): CompletionEntry {
    return {
        label: item.label,
        kind: item.kind ?? null,
        score: item.sort_text != null ? Number.parseFloat(item.sort_text) : 0,
        detail: item.label_details?.detail ?? null,
        description: item.label_details?.description ?? null,
        edit: item.text_edit != null ? rawRange(item.text_edit.range) : null,
        // Items without a text edit may still carry a bare insert_text
        // (import completion appends the closing semicolon through it).
        newText: item.text_edit?.new_text ?? item.insert_text ?? null,
        snippet: item.insert_text_format === "Snippet",
        deprecated: item.tags?.includes("Deprecated") ?? false,
    };
}

function replyCompletionEntry(item: proto.CompletionItem): CompletionEntry {
    const edit = item.textEdit;
    if (edit !== undefined && !("range" in edit)) {
        throw new Error("clice always replies with plain TextEdit completion edits");
    }
    return {
        label: item.label,
        kind: item.kind !== undefined ? enumName(proto.CompletionItemKind, item.kind) : null,
        score: item.sortText !== undefined ? Number.parseFloat(item.sortText) : 0,
        detail: item.labelDetails?.detail ?? null,
        description: item.labelDetails?.description ?? null,
        edit: edit !== undefined ? fmtRange(edit.range) : null,
        // Items without a text edit may still carry a bare insertText
        // (import completion appends the closing semicolon through it).
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
