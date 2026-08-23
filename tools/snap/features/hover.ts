import * as fs from "node:fs";
import * as path from "node:path";

import { markerPoints } from "../annotation.ts";
import { fmtRange, OffsetConverter, sortedMarkers, type Feature } from "../render.ts";
import { WORKSPACE_PLACEHOLDER } from "../snapshot.ts";

/// Hover cards may embed resolved filesystem paths (the include directive
/// card). Rewrite the corpus/workspace root to ${WS} and normalize the
/// separators of the rewritten span, so the two paths pin one portable
/// body. Only ${WS}-anchored spans are touched — a bare backslash
/// elsewhere is real content (macro line splices) — and the span ends at
/// whitespace, so a fixture path containing spaces would stay partly
/// native (none does).
function normalizeRoots(contents: string, root: string): string {
    const forms = [...new Set([root, fs.realpathSync.native(root)])]
        .flatMap((form) => [form.split(path.sep).join("/"), form])
        .sort((a, b) => b.length - a.length);
    let out = contents;
    for (const form of forms) {
        // Boundary-guarded on both sides: `/mnt/work` must not eat into
        // `/mnt/workspace`, and `/tmp/work` must not fire inside
        // `/var/tmp/work`. The leading guard must stay a lookbehind — real
        // matches sit mid-string after newlines and backticks, so a
        // consuming `(^|[\\/])` group would miss them all.
        const escaped = form.replace(/[.*+?^${}()|[\]\\]/g, String.raw`\$&`);
        out = out.replace(
            new RegExp(`(?<![\\w.~+-])${escaped}(?=$|[\\\\/])`, "g"),
            WORKSPACE_PLACEHOLDER,
        );
    }
    return out.replace(/\$\{WS\}[^\s`"')]*/g, (span) => span.replaceAll("\\", "/"));
}

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
            items.push([name, { rangeText, contents: normalizeRoots(raw.contents, ctx.root) }]);
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
                    contents: normalizeRoots(contents.value, ctx.root),
                },
            ]);
        }
        return formatHover(items);
    },
};
