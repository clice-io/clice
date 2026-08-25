/// Shared primitives of the snap render layer. Every feature renders
/// through exactly one formatter, fed by two thin adapters — fromInspect
/// for raw `clice inspect` payloads, fromServer for LSP replies — so the
/// two verification paths cannot drift in output shape, only in data.

import type * as proto from "vscode-languageserver-protocol";
import type { CliceClient } from "../client/client.ts";
import type { AnnotatedSource } from "./annotation.ts";

/// One file entry of `clice inspect` JSON output. Whole-document features
/// fill `result`; marker features fill `markers` keyed by annotation name
/// (null records a marker with no result, e.g. hover on whitespace).
/// Support files of a multi-file fixture carry only the hash.
export interface InspectFileEntry {
    stripped_hash: string;
    result?: unknown;
    markers?: Record<string, unknown> | null;
    error?: string | null;
    diagnostics?: string[] | null;
}

export interface InspectOutput {
    feature: string;
    files: Record<string, InspectFileEntry>;
}

/// What a feature adapter needs to render one file of a fixture.
export interface RenderContext {
    source: AnnotatedSource;
    stripped: Buffer;
    /// Root that file references normalize against: the corpus directory
    /// on the inspect path, the materialized workspace root on the server
    /// path — the same `${WS}`-relative names either way.
    root: string;
    /// Whether the fixture replays with background indexing enabled —
    /// server path only, the inspect path has no server and no index.
    indexing?: boolean;
}

/// The marker shape a feature consumes, deciding which files of a
/// multi-file fixture participate: point/completion features run per `§`
/// point, range features per `§⟦...⟧` range (whole document without one),
/// document features once per participating file.
export type FeatureShape = "document" | "point" | "range" | "completion";

export interface Feature {
    shape: FeatureShape;
    fromInspect: (entry: InspectFileEntry, ctx: RenderContext) => string[];
    fromServer: (client: CliceClient, uri: string, ctx: RenderContext) => Promise<string[]>;
}

export function fmtPos(pos: proto.Position): string {
    return `${pos.line}:${pos.character}`;
}

export function fmtRange(range: proto.Range): string {
    return `${fmtPos(range.start)}-${fmtPos(range.end)}`;
}

export function enumName(enumObject: Record<string, unknown>, value: number): string {
    for (const [name, v] of Object.entries(enumObject)) {
        if (v === value) {
            return name;
        }
    }
    return String(value);
}

/// C++ enum names arrive PascalCase; the server's semantic-tokens legend
/// lowercases the first letter, and snapshots use that reply spelling.
export function lowerFirst(name: string): string {
    return name.slice(0, 1).toLowerCase() + name.slice(1);
}

/// Byte offset -> LSP position over the stripped source. Lines are byte
/// ranges split at '\n'; characters count UTF-16 code units within the
/// line, matching the server's default position encoding.
export class OffsetConverter {
    private lineStarts: number[] = [0];
    private data: Buffer;

    // Plain field assignment: parameter properties are not erasable syntax,
    // and tools/ scripts run under bare `node` in strip-only TS mode.
    constructor(data: Buffer) {
        this.data = data;
        for (let i = 0; i < data.length; i++) {
            if (data[i] === 0x0a) {
                this.lineStarts.push(i + 1);
            }
        }
    }

    position(offset: number): { line: number; character: number } {
        let lo = 0;
        let hi = this.lineStarts.length - 1;
        while (lo < hi) {
            const mid = (lo + hi + 1) >> 1;
            if ((this.lineStarts[mid] ?? 0) <= offset) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        const start = this.lineStarts[lo] ?? 0;
        return { line: lo, character: this.data.subarray(start, offset).toString("utf8").length };
    }
}

/// Marker payloads ordered like the C++ side emitted them: named markers
/// sorted byte-wise, then `nameless_<i>` numerically.
export function sortedMarkers(markers: Record<string, unknown>): [string, unknown][] {
    const named: string[] = [];
    const nameless: string[] = [];
    for (const key of Object.keys(markers)) {
        (/^nameless_\d+$/.test(key) ? nameless : named).push(key);
    }
    named.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
    nameless.sort(
        (a, b) => Number(a.slice("nameless_".length)) - Number(b.slice("nameless_".length)),
    );
    return [...named, ...nameless].map((key) => [key, markers[key]]);
}

/// One `name:` section per marker, blank-line separated.
export function markerSections(
    markers: [string, unknown][],
    renderOne: (value: unknown) => string[],
): string[] {
    const out: string[] = [];
    for (const [name, value] of markers) {
        if (out.length > 0) {
            out.push("");
        }
        out.push(`${name}:`);
        out.push(...renderOne(value));
    }
    return out;
}

/// The A/B body of a fixture that carries a `config:` overlay: the same
/// content rendered under default options and under the overlay, so the
/// snapshot pins what the option actually changes. Blank section
/// separators stay unindented — no trailing whitespace in snapshots.
export function abBlocks(defaults: string[], configured: string[]): string[] {
    const indent = (line: string): string => (line === "" ? line : `  ${line}`);
    return ["default:", ...defaults.map(indent), "", "configured:", ...configured.map(indent)];
}

/// Partition a fixture's output by file:
///
///     --- lib.h
///     <lines>
///
///     --- main.cpp
///     <lines>
///
/// A single participating file keeps its plain body — the layout every
/// single-file fixture pins.
export function fileSections(sections: [string, string[]][]): string[] {
    const first = sections[0];
    if (sections.length === 1 && first) {
        return first[1];
    }
    const out: string[] = [];
    for (const [rel, lines] of sections) {
        if (out.length > 0) {
            out.push("");
        }
        out.push(`--- ${rel}`);
        out.push(...lines);
    }
    return out;
}

/// Completion and signature replies are open-ended, so a snapshot pins at
/// most ten items and announces the cut, so a truncated list never reads
/// as "nothing else matched".
export const SNAP_ITEM_LIMIT = 10;
