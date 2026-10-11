/// Shared by the one-off migration checks of S-NAV (navigation.ts,
/// workspace_symbol.ts, switch_source_header.ts): the old snapshots of a
/// snap corpus read back into entries, each place rendered the way the new
/// output names it, and the blocks of the new snap.toml files.

import * as fs from "node:fs";
import * as path from "node:path";
import { snapCorpora, type SnapCorpus, type SnapFixture } from "../snap/corpus.ts";
import { CASES_DIR } from "../testing/cases/project.ts";
import { renderLocation, SourceText, type Range } from "../testing/render/position.ts";
import { readToml, type Table } from "../testing/render/value.ts";

export function corpus(feature: string): SnapCorpus {
    const found = snapCorpora().find((c) => c.feature === feature);
    if (found === undefined) {
        throw new Error(`no snap corpus ${feature}`);
    }
    return found;
}

/// One `- { ... }` line of an old snapshot, its fields unquoted.
export interface OldEntry {
    section: string;
    fields: Record<string, string>;
}

/// One block of an old snapshot: a marker's (or query's, or file's)
/// sections, in the file of a unit fixture it lies in.
export interface OldBlock {
    file: string;
    name: string;
    none: boolean;
    entries: OldEntry[];
    /// `key: value` lines of the block itself (a counterpart's preferred).
    values: Record<string, string>;
}

function unquote(value: string): string {
    return value.startsWith('"') ? (JSON.parse(value) as string) : value;
}

function fieldsOf(body: string): Record<string, string> {
    const out: Record<string, string> = {};
    const re = /(\w+): ("(?:[^"\\]|\\.)*"|\[[^\]]*\]|[^,]+)(?:, |$)/g;
    for (const match of body.matchAll(re)) {
        out[match[1] ?? ""] = unquote(match[2] ?? "");
    }
    return out;
}

/// The old snapshot of an active fixture, block by block.
export function oldBlocks(c: SnapCorpus, fixture: SnapFixture): OldBlock[] {
    const file = path.join(c.corpus, fixture.rel.replace(/\.cpp$/, ".snap.yml"));
    const lines = fs.readFileSync(file, "utf8").replaceAll("\r\n", "\n").split("\n");
    let i = 1;
    while (lines[i] !== "---") {
        i += 1;
    }
    const out: OldBlock[] = [];
    let current: OldBlock | undefined;
    let section = "";
    let unit = "";
    for (i += 1; i < lines.length; i += 1) {
        const line = lines[i] ?? "";
        let match: RegExpExecArray | null;
        if (line.trim() === "") {
            continue;
        } else if ((match = /^--- (.*)$/.exec(line))) {
            unit = match[1] ?? "";
        } else if ((match = /^("(?:[^"\\]|\\.)*"|[^ "].*?):( none)?$/.exec(line))) {
            current = {
                file: unit,
                name: unquote(match[1] ?? ""),
                none: match[2] !== undefined,
                entries: [],
                values: {},
            };
            out.push(current);
        } else if ((match = /^ {2}(\w+):( .*)?$/.exec(line)) && current !== undefined) {
            if (match[2] === undefined) {
                section = match[1] ?? "";
            } else {
                current.values[match[1] ?? ""] = unquote(match[2].trim());
            }
        } else if ((match = /^ {2,6}- \{ (.*) \}$/.exec(line)) && current !== undefined) {
            current.entries.push({ section, fields: fieldsOf(match[1] ?? "") });
        } else {
            throw new Error(`${file}:${i + 1}: unread line: ${line}`);
        }
    }
    return out;
}

/// `l:c-l:c` as a range.
export function rangeOf(text: string): Range {
    const match = /^(\d+):(\d+)-(\d+):(\d+)$/.exec(text);
    if (match === null) {
        throw new Error(`not a range: ${text}`);
    }
    const [line, character, endLine, endCharacter] = match.slice(1).map(Number);
    return {
        start: { line: line ?? 0, character: character ?? 0 },
        end: { line: endLine ?? 0, character: endCharacter ?? 0 },
    };
}

/// The old fixture's files as they compiled, under the names the new case
/// gives them.
export class OldFiles {
    private readonly files = new Map<string, SourceText>();
    /// Old workspace path (`${WS}/...`) prefix the fixture's files sit under.
    private readonly base: string;
    private readonly entry: string;
    /// The entry's name in the new case.
    private readonly entryName: string;

    constructor(c: SnapCorpus, fixture: SnapFixture, entryName = "main.cpp") {
        this.base = fixture.unit === "" ? path.posix.dirname(fixture.rel) : fixture.unit;
        this.entry = fixture.unit === "" ? path.posix.basename(fixture.rel) : "";
        this.entryName = entryName;
        const add = (rel: string, text: string): void => {
            const name = this.newPath(`\${WS}/${rel}`);
            this.files.set(name, new SourceText(name, text));
        };
        for (const file of fixture.files) {
            add(file.rel, file.source.content);
        }
        for (const extra of fixture.extras) {
            add(extra, fs.readFileSync(path.join(c.corpus, extra), "utf8"));
        }
    }

    /// An old snapshot path's name in the new case.
    newPath(old: string): string {
        let rel = old.replace(/^\$\{WS\}\//, "");
        if (this.base !== "." && this.base !== "") {
            rel = rel.slice(this.base.length + 1);
        }
        return this.entry !== "" && rel === this.entry ? this.entryName : rel;
    }

    /// A `--- file` head's name in the new case.
    unitPath(head: string): string {
        if (head === "") {
            return this.entryName;
        }
        return this.entry !== "" && head === this.entry ? this.entryName : head;
    }

    /// The place as the new output names it, seen from `query`, its offset
    /// suffix dropped.
    place(query: string, oldPath: string, range: string): string {
        const location = { path: this.newPath(oldPath), range: rangeOf(range) };
        const scope = {
            file: (name: string): SourceText => {
                const source = this.files.get(name);
                if (source === undefined) {
                    throw new Error(`no old file ${name}`);
                }
                return source;
            },
            query: { path: query },
        };
        return stripOffset(renderLocation(location, scope));
    }
}

const OFFSET = / @[+-]?\d+$/;

export function stripOffset(place: string): string {
    return place.replace(OFFSET, "");
}

/// The blocks of a case's snap.toml, by test and label.
export function newBlocks(caseId: string): Map<string, Map<string, Table>> {
    const file = path.join(CASES_DIR, caseId, "snap.toml");
    const out = new Map<string, Map<string, Table>>();
    if (!fs.existsSync(file)) {
        return out;
    }
    for (const [test, blocks] of Object.entries(readToml(fs.readFileSync(file, "utf8")))) {
        out.set(test, new Map(Object.entries(blocks as Record<string, Table>)));
    }
    return out;
}

/// Multiset difference: what `a` holds that `b` does not, as many times.
export function missing(a: readonly string[], b: readonly string[]): string[] {
    const left = new Map<string, number>();
    for (const item of b) {
        left.set(item, (left.get(item) ?? 0) + 1);
    }
    const out: string[] = [];
    for (const item of a) {
        const count = left.get(item) ?? 0;
        if (count > 0) {
            left.set(item, count - 1);
        } else {
            out.push(item);
        }
    }
    return out;
}
