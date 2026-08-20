import * as proto from "vscode-languageserver-protocol";
import { markerRanges } from "../annotation.ts";
import {
    enumName,
    fmtPos,
    markerSections,
    OffsetConverter,
    sortedMarkers,
    type Feature,
} from "../render.ts";
import { yamlStr } from "../snapshot.ts";

interface HintEntry {
    pos: string;
    kind: string;
    label: string;
    paddingLeft: boolean;
    paddingRight: boolean;
}

interface RawInlayHint {
    offset: number;
    kind: string;
    label: string;
    padding_left: boolean;
    padding_right: boolean;
}

/// Twin of the HintCategory -> protocol::InlayHintKind switch in
/// src/feature/inlay_hints.cpp.
const LSP_INLAY_KIND: Record<string, string> = {
    Parameter: "Parameter",
    DefaultArgument: "Parameter",
    Type: "Type",
    Designator: "Type",
    BlockEnd: "Type",
};

function formatInlayHints(hints: HintEntry[]): string[] {
    return hints.map((hint) => {
        let line = `- { pos: "${hint.pos}"`;
        line += `, kind: ${hint.kind}`;
        line += `, label: ${yamlStr(hint.label)}`;
        if (hint.paddingLeft) {
            line += ", padding_left: true";
        }
        if (hint.paddingRight) {
            line += ", padding_right: true";
        }
        return line + " }";
    });
}

function adaptRaw(result: unknown, map: OffsetConverter): HintEntry[] {
    return (result as RawInlayHint[]).map((hint) => {
        const kind = LSP_INLAY_KIND[hint.kind];
        if (kind === undefined) {
            throw new Error(`unmapped inlay hint kind '${hint.kind}'; extend LSP_INLAY_KIND`);
        }
        return {
            pos: fmtPos(map.position(hint.offset)),
            kind,
            label: hint.label,
            paddingLeft: hint.padding_left,
            paddingRight: hint.padding_right,
        };
    });
}

function adaptReply(hints: proto.InlayHint[]): HintEntry[] {
    return hints.map((hint) => {
        if (hint.kind === undefined) {
            throw new Error("clice always replies with an inlay hint kind");
        }
        return {
            pos: fmtPos(hint.position),
            kind: enumName(proto.InlayHintKind, hint.kind),
            label:
                typeof hint.label === "string"
                    ? hint.label
                    : hint.label.map((part) => part.value).join(""),
            paddingLeft: hint.paddingLeft ?? false,
            paddingRight: hint.paddingRight ?? false,
        };
    });
}

export const inlayHint: Feature = {
    shape: "range",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        // A fixture may scope the request with `§⟦...⟧` range markers — one
        // section per marker; without them the whole document is requested.
        if (entry.markers != null) {
            return markerSections(sortedMarkers(entry.markers), (value) =>
                formatInlayHints(adaptRaw(value, map)),
            );
        }
        return formatInlayHints(adaptRaw(entry.result, map));
    },
    async fromServer(client, uri, ctx) {
        const ranges = markerRanges(ctx.source);
        if (ranges.length === 0) {
            const wholeFile: proto.Range = {
                start: { line: 0, character: 0 },
                end: { line: ctx.source.content.split("\n").length, character: 0 },
            };
            return formatInlayHints(adaptReply((await client.inlayHints(uri, wholeFile)) ?? []));
        }
        const map = new OffsetConverter(ctx.stripped);
        const sections: [string, unknown][] = [];
        for (const [name, [begin, end]] of ranges) {
            const range: proto.Range = {
                start: map.position(begin),
                end: map.position(end),
            };
            sections.push([name, (await client.inlayHints(uri, range)) ?? []]);
        }
        return markerSections(sections, (value) =>
            formatInlayHints(adaptReply(value as proto.InlayHint[])),
        );
    },
};
