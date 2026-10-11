/// Shared by the one-off migration checks of S-HOVER (hover.ts,
/// signature_help.ts, document_symbol.ts): the old snapshots of a snap
/// corpus read back into facts, and the blocks of the new snap.toml files
/// under a tests-next topic. Each check renders an old fact the way the
/// new output names it and looks for it in the case it moved to.

import * as fs from "node:fs";
import * as path from "node:path";
import { snapCorpora, type SnapCorpus, type SnapFixture } from "../snap/corpus.ts";
import { parseSnap } from "../snap/snapshot.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { renderLocation, SourceText, type Range } from "../testing/render/position.ts";
import { readToml } from "../testing/render/value.ts";

export function corpus(feature: string): SnapCorpus {
    const found = snapCorpora().find((c) => c.feature === feature);
    if (found === undefined) {
        throw new Error(`no snap corpus ${feature}`);
    }
    return found;
}

/// Old snapshot variants: the plain one, a config fixture's two halves, or
/// a separate fixture's two paths.
export type Variant = "" | "default" | "configured" | "inspect" | "server";

/// One old snapshot body, per variant and file, as lines.
export interface OldBody {
    variant: Variant;
    file: string;
    lines: string[];
}

function snapshotFiles(c: SnapCorpus, fixture: SnapFixture): { variant: Variant; file: string }[] {
    const base = path.join(c.corpus, fixture.rel.replace(/\.cpp$/, ""));
    if (fixture.meta.snap === "separate") {
        return [
            { variant: "inspect", file: `${base}.inspect.snap.yml` },
            { variant: "server", file: `${base}.server.snap.yml` },
        ];
    }
    return [{ variant: "", file: `${base}.snap.yml` }];
}

/// The name a file of `fixture` has in its old snapshot's `--- file` heads.
export function fileName(fixture: SnapFixture, rel: string): string {
    return fixture.unit === "" ? path.posix.basename(rel) : rel.slice(fixture.unit.length + 1);
}

/// The snapshot bodies of an active fixture, split by variant and file.
export function oldBodies(c: SnapCorpus, fixture: SnapFixture): OldBody[] {
    const out: OldBody[] = [];
    for (const { variant, file } of snapshotFiles(c, fixture)) {
        const snap = parseSnap(fs.readFileSync(file, "utf8"));
        if (snap === null) {
            throw new Error(`${file}: no snapshot front matter`);
        }
        let halves: { variant: Variant; lines: string[] }[] = [
            { variant, lines: snap.body.split("\n") },
        ];
        if (fixture.meta.config !== undefined) {
            const lines = snap.body.split("\n");
            const split = lines.indexOf("configured:");
            if (lines[0] !== "default:" || split < 0) {
                throw new Error(`${file}: expected default: and configured: halves`);
            }
            const unindent = (part: string[]): string[] =>
                part.map((line) => (line.startsWith("  ") ? line.slice(2) : line));
            halves = [
                { variant: "default", lines: unindent(lines.slice(1, split)) },
                { variant: "configured", lines: unindent(lines.slice(split + 1)) },
            ];
        }
        for (const half of halves) {
            const entry = fileName(fixture, fixture.rel);
            let current = entry;
            let lines: string[] = [];
            const flush = (): void => {
                while (lines.length > 0 && lines[lines.length - 1] === "") {
                    lines.pop();
                }
                if (lines.length > 0) {
                    out.push({ variant: half.variant, file: current, lines });
                }
            };
            for (const line of half.lines) {
                const head = /^--- (\S+)$/.exec(line);
                if (head?.[1] !== undefined) {
                    flush();
                    current = head[1];
                    lines = [];
                    continue;
                }
                lines.push(line);
            }
            flush();
        }
    }
    return out;
}

/// The old file's text with markers stripped, by its snapshot name.
export function oldSource(fixture: SnapFixture, name: string): SourceText {
    const file = fixture.files.find((f) => fileName(fixture, f.rel) === name);
    if (file === undefined) {
        throw new Error(`${fixture.rel}: no file ${name}`);
    }
    return new SourceText(name, file.source.content);
}

/// A `L:C-L:C` range of the old snapshots.
export function parseRange(text: string): Range {
    const m = /^(\d+):(\d+)-(\d+):(\d+)$/.exec(text);
    if (m === null) {
        throw new Error(`not a range: ${text}`);
    }
    const [, a, b, c, d] = m.map(Number);
    return {
        start: { line: a ?? 0, character: b ?? 0 },
        end: { line: c ?? 0, character: d ?? 0 },
    };
}

/// A rendering without its disambiguating offset, which depends on the
/// rest of the file.
export function bare(text: string): string {
    return text.replace(/ @[+-]?\d+$/, "");
}

/// `range` of `source` as the new output names it, offset dropped.
export function renderOld(source: SourceText, range: Range): string {
    return bare(
        renderLocation(
            { path: source.path, range },
            { file: () => source, query: { path: source.path, line: range.start.line } },
        ),
    );
}

/// The marker name an old one became: its `NN_` order prefix dropped.
export function renamed(marker: string): string {
    return marker.replace(/^\d+_/, "");
}

/// One block of a new snap.toml: `[<test>.<label>]`, or under a client for
/// paths pinned apart.
export interface NewBlock {
    caseId: string;
    test: string;
    client?: "inspect" | "server";
    label: string;
    table: Record<string, unknown>;
}

function isRecord(value: unknown): value is Record<string, unknown> {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

/// Every block of every snap.toml under `topic`, by case id.
export function newBlocks(topic: string): Map<string, NewBlock[]> {
    const out = new Map<string, NewBlock[]>();
    const walk = (dir: string): void => {
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                walk(full);
            } else if (entry.name === "snap.toml") {
                const caseId = path.relative(CASES_DIR, dir).split(path.sep).join("/");
                const blocks: NewBlock[] = [];
                const tables = readToml(fs.readFileSync(full, "utf8"));
                for (const [test, labels] of Object.entries(tables)) {
                    if (!isRecord(labels)) {
                        continue;
                    }
                    const clients = Object.keys(labels);
                    const apart =
                        clients.length === 2 &&
                        clients.includes("inspect") &&
                        clients.includes("server");
                    const groups: [("inspect" | "server") | undefined, Record<string, unknown>][] =
                        apart
                            ? [
                                  ["inspect", labels["inspect"] as Record<string, unknown>],
                                  ["server", labels["server"] as Record<string, unknown>],
                              ]
                            : [[undefined, labels]];
                    for (const [client, group] of groups) {
                        for (const [label, table] of Object.entries(group)) {
                            if (isRecord(table)) {
                                blocks.push({
                                    caseId,
                                    test,
                                    ...(client === undefined ? {} : { client }),
                                    label,
                                    table,
                                });
                            }
                        }
                    }
                }
                out.set(caseId, blocks);
            }
        }
    };
    walk(path.join(CASES_DIR, topic));
    return out;
}

/// The test an old variant's facts live under in the new case.
export function testOf(variant: Variant): { test: string; client?: "inspect" | "server" } {
    switch (variant) {
        case "":
            return { test: "answers" };
        case "default":
        case "configured":
            return { test: variant };
        case "inspect":
        case "server":
            return { test: "answers", client: variant };
    }
}

/// Where an old fixture went: the cases holding its markers (the first one
/// unless `markers` names another), or dropped for the case that covers it.
export interface Destination {
    /// The cases its markers went to; a marker is found by its label.
    cases: string[];
    /// Marker → the name it has now, where not its renamed form.
    renames?: Record<string, string>;
    /// Markers left out, each with the case that pins the same.
    dropped?: Record<string, string>;
    /// The new case's test the facts are under, where not the variant's.
    test?: string;
}

/// Facts that differ on purpose: `<fixture>#<marker>` (with `@<variant>`
/// for a config or separate half) → why.
export type Explained = Record<string, string>;

export interface Tally {
    checked: number;
    explained: number;
    dropped: number;
    unexplained: string[];
}

export function report(feature: string, tally: Tally, oldAsked: number, newAsked: number): void {
    console.log(
        `${feature}: ${tally.checked} old facts found as they were, ${tally.explained} changed on ` +
            `purpose, ${tally.dropped} dropped for a covering case`,
    );
    console.log(`${feature}: asked ${oldAsked} times before, ${newAsked} now`);
    if (tally.unexplained.length > 0) {
        console.log(`${tally.unexplained.length} unexplained differences:`);
        for (const line of tally.unexplained) {
            console.log(`  ${line}`);
        }
        process.exitCode = 1;
    } else {
        console.log("no unexplained differences");
    }
}

/// Where each old marker went, for the report's mapping table: run a check
/// with `--table`.
export class Mapping {
    private readonly rows = new Map<string, Map<string, string[]>>();

    /// `marker` of `fixture` is now `where` (a case and label, or a drop).
    record(fixture: string, marker: string, where: string): void {
        const markers = this.rows.get(fixture) ?? new Map<string, string[]>();
        const list = markers.get(where) ?? [];
        list.push(marker);
        markers.set(where, list);
        this.rows.set(fixture, markers);
    }

    print(title: string): void {
        if (!process.argv.includes("--table")) {
            return;
        }
        console.log(`\n| ${title} | old markers | now |`);
        console.log("| --- | --- | --- |");
        for (const [fixture, markers] of [...this.rows].sort(([a], [b]) => (a < b ? -1 : 1))) {
            for (const [where, list] of markers) {
                console.log(`| ${fixture} | ${[...new Set(list)].join(", ")} | ${where} |`);
            }
        }
    }
}
