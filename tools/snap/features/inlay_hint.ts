import * as proto from "vscode-languageserver-protocol";
import { markerRanges } from "../annotation.ts";
import type { CliceClient } from "../../client/client.ts";
import {
    enumName,
    fmtPos,
    fmtRange,
    markerSections,
    OffsetConverter,
    sortedMarkers,
    type Feature,
} from "../render.ts";
import { normalizeFilePath, normalizeFileUri, yamlStr } from "../snapshot.ts";

interface HintEntry {
    pos: string;
    kind: string;
    label: string;
    /// The label pieces carrying a location, as `<piece> <file>:<range>`.
    links: string[];
    paddingLeft: boolean;
    paddingRight: boolean;
}

interface RawInlayHintPart {
    value: string;
    location?: { path: string; range: proto.Range } | null;
}

interface RawInlayHint {
    offset: number;
    kind: string;
    label: RawInlayHintPart[];
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
        if (hint.links.length > 0) {
            line += `, links: [${hint.links.map(yamlStr).join(", ")}]`;
        }
        if (hint.paddingLeft) {
            line += ", padding_left: true";
        }
        if (hint.paddingRight) {
            line += ", padding_right: true";
        }
        return line + " }";
    });
}

function linkEntry(value: string, file: string, range: proto.Range): string {
    return `${value} ${file}:${fmtRange(range)}`;
}

function adaptRaw(result: unknown, map: OffsetConverter, root: string): HintEntry[] {
    return (result as RawInlayHint[]).map((hint) => {
        const kind = LSP_INLAY_KIND[hint.kind];
        if (kind === undefined) {
            throw new Error(`unmapped inlay hint kind '${hint.kind}'; extend LSP_INLAY_KIND`);
        }
        return {
            pos: fmtPos(map.position(hint.offset)),
            kind,
            label: hint.label.map((part) => part.value).join(""),
            links: hint.label.flatMap((part) =>
                part.location != null
                    ? [
                          linkEntry(
                              part.value,
                              normalizeFilePath(part.location.path, root),
                              part.location.range,
                          ),
                      ]
                    : [],
            ),
            paddingLeft: hint.padding_left,
            paddingRight: hint.padding_right,
        };
    });
}

function adaptReply(hints: proto.InlayHint[], root: string): HintEntry[] {
    return hints.map((hint) => {
        if (hint.kind === undefined) {
            throw new Error("clice always replies with an inlay hint kind");
        }
        const parts: proto.InlayHintLabelPart[] =
            typeof hint.label === "string" ? [{ value: hint.label }] : hint.label;
        return {
            pos: fmtPos(hint.position),
            kind: enumName(proto.InlayHintKind, hint.kind),
            label: parts.map((part) => part.value).join(""),
            links: parts.flatMap((part) =>
                part.location != null
                    ? [
                          linkEntry(
                              part.value,
                              normalizeFileUri(part.location.uri, root),
                              part.location.range,
                          ),
                      ]
                    : [],
            ),
            paddingLeft: hint.paddingLeft ?? false,
            paddingRight: hint.paddingRight ?? false,
        };
    });
}

/// The hints as the editor holds them once it resolved each label naming
/// symbols: the snap client resolves `label.location` lazily, as VS Code
/// does.
async function resolved(client: CliceClient, hints: proto.InlayHint[] | null) {
    return Promise.all(
        (hints ?? []).map(async (hint) =>
            hint.data === undefined
                ? hint
                : client.sendRequest(proto.InlayHintResolveRequest.type, hint),
        ),
    );
}

export const inlayHint: Feature = {
    shape: "range",
    fromInspect(entry, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        // A fixture may scope the request with `§⟦...⟧` range markers — one
        // section per marker; without them the whole document is requested.
        if (entry.markers != null) {
            return markerSections(sortedMarkers(entry.markers), (value) =>
                formatInlayHints(adaptRaw(value, map, ctx.root)),
            );
        }
        return formatInlayHints(adaptRaw(entry.result, map, ctx.root));
    },
    async fromServer(client, uri, ctx) {
        const ranges = markerRanges(ctx.source);
        if (ranges.length === 0) {
            const wholeFile: proto.Range = {
                start: { line: 0, character: 0 },
                end: { line: ctx.source.content.split("\n").length, character: 0 },
            };
            const hints = await resolved(client, await client.inlayHints(uri, wholeFile));
            return formatInlayHints(adaptReply(hints, ctx.root));
        }
        const map = new OffsetConverter(ctx.stripped);
        const sections: [string, unknown][] = [];
        for (const [name, [begin, end]] of ranges) {
            const range: proto.Range = {
                start: map.position(begin),
                end: map.position(end),
            };
            sections.push([name, await resolved(client, await client.inlayHints(uri, range))]);
        }
        return markerSections(sections, (value) =>
            formatInlayHints(adaptReply(value as proto.InlayHint[], ctx.root)),
        );
    },
};
