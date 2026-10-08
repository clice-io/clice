import type * as proto from "vscode-languageserver-protocol";
import { markerPoints } from "../annotation.ts";
import { fmtRange, OffsetConverter, sortedMarkers, type Feature } from "../render.ts";
import { yamlStr } from "../snapshot.ts";

interface RawRange {
    begin: number;
    end: number;
}

const EXCERPT_LIMIT = 48;

/// A range's text with each run of whitespace folded into one space, cut
/// short past EXCERPT_LIMIT: enough to tell the steps apart at a glance.
function excerpt(lines: string[], range: proto.Range): string {
    const { start, end } = range;
    let text =
        lines[start.line]?.slice(
            start.character,
            start.line === end.line ? end.character : undefined,
        ) ?? "";
    for (let line = start.line + 1; line <= end.line; line += 1) {
        text += "\n" + (lines[line]?.slice(0, line === end.line ? end.character : undefined) ?? "");
    }
    const folded = text.replace(/\s+/g, " ");
    return folded.length > EXCERPT_LIMIT ? folded.slice(0, EXCERPT_LIMIT) + "…" : folded;
}

function formatChains(items: [string, proto.Range[]][], stripped: Buffer): string[] {
    const lines = stripped.toString("utf8").split("\n");
    const out: string[] = [];
    for (const [name, chain] of items) {
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        for (const range of chain) {
            out.push(
                `  - { range: "${fmtRange(range)}", text: ${yamlStr(excerpt(lines, range))} }`,
            );
        }
    }
    return out;
}

/// The selection ranges at each `§`-marker, innermost first, one block per
/// marker; the server path asks for every marker in one request.
///
///     name:
///       - { range: "6:8-6:13", text: "total" }
///       - { range: "6:4-6:28", text: "int total = a * (b + 1);" }
export const selectionRange: Feature = {
    shape: "point",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return formatChains(
            sortedMarkers(entry.markers ?? {}).map(([name, value]) => [
                name,
                (value as RawRange[]).map((raw) => ({
                    start: map.position(raw.begin),
                    end: map.position(raw.end),
                })),
            ]),
            ctx.stripped,
        );
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const points = markerPoints(ctx.source);
        const reply = await client.selectionRanges(
            uri,
            points.map(([, offset]) => map.position(offset)),
        );
        if (reply?.length !== points.length) {
            throw new Error("clice answers every position of a selectionRange request");
        }
        return formatChains(
            points.map(([name], i) => {
                const chain: proto.Range[] = [];
                for (
                    let step: proto.SelectionRange | undefined = reply[i];
                    step;
                    step = step.parent
                ) {
                    chain.push(step.range);
                }
                return [name, chain];
            }),
            ctx.stripped,
        );
    },
};
