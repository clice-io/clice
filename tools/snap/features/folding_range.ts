import type * as proto from "vscode-languageserver-protocol";
import { OffsetConverter, type Feature } from "../render.ts";
import { yamlStr } from "../snapshot.ts";

interface FoldingEntry {
    range: string;
    kind: string | null;
    collapsedText: string;
}

interface RawFoldingRange {
    range: { begin: number; end: number };
    kind?: string | null;
    collapsed_text: string;
}

function formatFoldingRanges(entries: FoldingEntry[]): string[] {
    return entries.map((entry) => {
        let line = `- { range: "${entry.range}"`;
        if (entry.kind !== null) {
            line += `, kind: ${entry.kind}`;
        }
        if (entry.collapsedText) {
            line += `, collapsed_text: ${yamlStr(entry.collapsedText)}`;
        }
        return line + " }";
    });
}

export const foldingRange: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return formatFoldingRanges(
            (entry.result as RawFoldingRange[]).map((raw) => {
                const start = map.position(raw.range.begin);
                const end = map.position(raw.range.end);
                return {
                    range: `${start.line}:${start.character}-${end.line}:${end.character}`,
                    kind: raw.kind ?? null,
                    collapsedText: raw.collapsed_text,
                };
            }),
        );
    },
    async fromServer(client, uri) {
        const ranges: proto.FoldingRange[] = (await client.foldingRanges(uri)) ?? [];
        return formatFoldingRanges(
            ranges.map((range) => ({
                range: `${range.startLine}:${range.startCharacter}-${range.endLine}:${range.endCharacter}`,
                kind: range.kind ?? null,
                collapsedText: range.collapsedText ?? "",
            })),
        );
    },
};
