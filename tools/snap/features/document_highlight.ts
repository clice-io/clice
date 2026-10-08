import * as proto from "vscode-languageserver-protocol";
import { markerPoints } from "../annotation.ts";
import { enumName, fmtRange, OffsetConverter, sortedMarkers, type Feature } from "../render.ts";

/// The highlights at each `§`-marker, one block per marker in range order:
///
///     name:
///       - { range: "3:4-3:9", kind: Write }
///
///     other: none

interface HighlightEntry {
    start: proto.Position;
    end: proto.Position;
    kind: string;
}

interface RawHighlight {
    range: { begin: number; end: number };
    kind: string;
}

function formatHighlights(items: [string, HighlightEntry[]][]): string[] {
    const out: string[] = [];
    for (const [name, entries] of items) {
        if (out.length > 0) {
            out.push("");
        }
        if (entries.length === 0) {
            out.push(`${name}: none`);
            continue;
        }
        out.push(`${name}:`);
        const sorted = [...entries].sort(
            (a, b) =>
                a.start.line - b.start.line ||
                a.start.character - b.start.character ||
                a.end.line - b.end.line ||
                a.end.character - b.end.character,
        );
        for (const entry of sorted) {
            out.push(
                `  - { range: "${fmtRange({ start: entry.start, end: entry.end })}", kind: ${entry.kind} }`,
            );
        }
    }
    return out;
}

export const documentHighlight: Feature = {
    shape: "point",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return formatHighlights(
            sortedMarkers(entry.markers ?? {}).map(([name, value]) => [
                name,
                (value as RawHighlight[]).map((raw) => ({
                    start: map.position(raw.range.begin),
                    end: map.position(raw.range.end),
                    kind: raw.kind,
                })),
            ]),
        );
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const items: [string, HighlightEntry[]][] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const { line, character } = map.position(offset);
            const reply = (await client.documentHighlightAt(uri, line, character)) ?? [];
            items.push([
                name,
                reply.map((highlight) => ({
                    start: highlight.range.start,
                    end: highlight.range.end,
                    kind: enumName(
                        proto.DocumentHighlightKind,
                        highlight.kind ?? proto.DocumentHighlightKind.Text,
                    ),
                })),
            ]);
        }
        return formatHighlights(items);
    },
};
