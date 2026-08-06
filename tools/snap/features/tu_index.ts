import { OffsetConverter, type Feature } from "../render.ts";
import { yamlStr } from "../snapshot.ts";

/// The TU-index occurrence dump has no LSP request shape, so tu_index
/// fixtures are `verify: inspect` and the server adapter is unreachable.
interface RawOccurrence {
    range: { begin: number; end: number };
    kind: string;
    relations: string[];
}

export const tuIndex: Feature = {
    shape: "document",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        return (entry.result as RawOccurrence[]).map((occurrence) => {
            const pos = map.position(occurrence.range.begin);
            const text = ctx.stripped
                .subarray(occurrence.range.begin, occurrence.range.end)
                .toString("utf8");
            let line =
                `- { loc: "${pos.line}:${pos.character}", ` +
                `kind: ${occurrence.kind}, text: ${yamlStr(text)}`;
            if (occurrence.relations.length > 0) {
                line += `, relations: [${occurrence.relations.join(", ")}]`;
            }
            return line + " }";
        });
    },
    fromServer() {
        return Promise.reject(new Error("tu_index has no server path; use verify: inspect"));
    },
};
