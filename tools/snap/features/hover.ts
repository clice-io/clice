import { markerPoints } from "../annotation.ts";
import { fmtRange, OffsetConverter, sortedMarkers, type Feature } from "../render.ts";

type HoverItem = { rangeText: string | null; contents: string } | null;

interface RawHoverResult {
    range: { begin: number; end: number } | null;
    contents: string;
}

/// One rendered hover block per marker:
///
///     name: { range: "1:4-1:9" }
///     <markdown>
///
///     other: NO HOVER
function formatHover(items: [string, HoverItem][]): string[] {
    const out: string[] = [];
    for (const [name, item] of items) {
        if (out.length > 0) {
            out.push("");
        }
        if (item === null) {
            out.push(`${name}: NO HOVER`);
            continue;
        }
        out.push(`${name}:${item.rangeText === null ? "" : ` { range: "${item.rangeText}" }`}`);
        out.push(...item.contents.split("\n"));
    }
    return out;
}

export const hover: Feature = {
    shape: "point",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const items: [string, HoverItem][] = [];
        for (const [name, value] of sortedMarkers(entry.markers ?? {})) {
            if (value === null) {
                items.push([name, null]);
                continue;
            }
            const raw = value as RawHoverResult;
            let rangeText: string | null = null;
            if (raw.range !== null) {
                const start = map.position(raw.range.begin);
                const end = map.position(raw.range.end);
                rangeText = `${start.line}:${start.character}-${end.line}:${end.character}`;
            }
            items.push([name, { rangeText, contents: raw.contents }]);
        }
        return formatHover(items);
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const items: [string, HoverItem][] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const pos = map.position(offset);
            const reply = await client.hoverAt(uri, pos.line, pos.character);
            if (!reply) {
                items.push([name, null]);
                continue;
            }
            const contents = reply.contents;
            if (typeof contents === "string" || Array.isArray(contents)) {
                throw new Error("clice always replies with MarkupContent hover contents");
            }
            items.push([
                name,
                {
                    rangeText: reply.range ? fmtRange(reply.range) : null,
                    contents: contents.value,
                },
            ]);
        }
        return formatHover(items);
    },
};
