import type * as proto from "vscode-languageserver-protocol";
import { markerPoints } from "../annotation.ts";
import {
    markerSections,
    OffsetConverter,
    SNAP_ITEM_LIMIT,
    sortedMarkers,
    type Feature,
} from "../render.ts";

/// A signature reduced to what the snapshot pins: the rendered label with
/// the active parameter bracketed in place (`foo(int x, ⟦int y⟧)`), which
/// pins both the parameter offsets and the active index in one line.
export interface SignatureEntry {
    label: string;
    /// [begin, end) substrings of `label`, one per parameter.
    parameters: [number, number][];
    activeParameter: number | null;
}

interface RawSignature {
    label: string;
    parameters?: { label: [number, number] }[] | null;
    active_parameter?: number | null;
}

export function signatureLines(signatures: SignatureEntry[]): string[] {
    if (signatures.length === 0) {
        return ["NO SIGNATURES"];
    }
    const out: string[] = [];
    for (const signature of signatures.slice(0, SNAP_ITEM_LIMIT)) {
        let label = signature.label;
        const active =
            signature.activeParameter !== null
                ? signature.parameters[signature.activeParameter]
                : undefined;
        if (active !== undefined) {
            const [begin, end] = active;
            label = `${label.slice(0, begin)}⟦${label.slice(begin, end)}⟧${label.slice(end)}`;
        }
        out.push(`- ${label}`);
    }
    if (signatures.length > SNAP_ITEM_LIMIT) {
        out.push(`… +${signatures.length - SNAP_ITEM_LIMIT} more`);
    }
    return out;
}

function rawSignatureEntry(signature: RawSignature): SignatureEntry {
    return {
        label: signature.label,
        parameters: (signature.parameters ?? []).map((parameter) => parameter.label),
        activeParameter: signature.active_parameter ?? null,
    };
}

function replySignatureEntry(signature: proto.SignatureInformation): SignatureEntry {
    const parameters: [number, number][] = [];
    for (const parameter of signature.parameters ?? []) {
        if (typeof parameter.label === "string") {
            throw new Error("clice always replies with [begin, end) parameter labels");
        }
        parameters.push(parameter.label);
    }
    return {
        label: signature.label,
        parameters,
        activeParameter: signature.activeParameter ?? null,
    };
}

export const signatureHelp: Feature = {
    shape: "completion",
    fromInspect(entry) {
        return markerSections(sortedMarkers(entry.markers ?? {}), (value) =>
            signatureLines(
                ((value as { signatures?: RawSignature[] | null }).signatures ?? []).map(
                    rawSignatureEntry,
                ),
            ),
        );
    },
    async fromServer(client, uri, ctx) {
        const map = new OffsetConverter(ctx.stripped);
        const sections: [string, unknown][] = [];
        for (const [name, offset] of markerPoints(ctx.source)) {
            const pos = map.position(offset);
            const reply = await client.signatureHelpAt(uri, pos.line, pos.character);
            sections.push([name, reply?.signatures ?? []]);
        }
        return markerSections(sections, (value) =>
            signatureLines((value as proto.SignatureInformation[]).map(replySignatureEntry)),
        );
    },
};
