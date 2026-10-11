/// A documented case as the docs site draws it: its page, its sources with
/// the markers taken out and their offsets kept, and its output as the run
/// left it, every place with the range it came from, so the site draws
/// answers into the code without reading snap.toml's text. The cases run
/// writes one per documented case under CLICE_CARDS_DIR, `<id>.json`.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "../../snap/annotation.ts";
import type { Case } from "../cases/case.ts";
import { statusOf, writeSnapshot, type Snapshot } from "../cases/output.ts";
import { MARKED, copyName } from "../cases/project.ts";
import type { Range } from "../render/position.ts";
import { Place, isList, isTable, type Value } from "../render/value.ts";

export interface CardFile {
    path: string;
    text: string;
    /// UTF-16 offsets into the text.
    markers: { name: string; begin: number; end: number; range: boolean }[];
}

/// A value of the output, a place as `{ $place, path, range }`.
export type CardValue =
    | string
    | number
    | boolean
    | { $place: string; path: string; range: Range }
    | CardValue[]
    | { [key: string]: CardValue };

export interface CardBlock {
    label: string;
    /// The marker the block answers at, when its label names one.
    marker?: string;
    value: CardValue;
}

export interface Card {
    id: string;
    topic: string;
    subtopic: string;
    title: string;
    summary: string;
    prose: string;
    status: string;
    issues: string[];
    files: CardFile[];
    tests: { name: string; blocks: CardBlock[] }[];
}

function filesOf(c: Case): CardFile[] {
    return c.inputs.map((rel) => {
        const name = copyName(rel);
        const raw = fs.readFileSync(path.join(c.dir, rel), "utf8");
        const text = MARKED.test(name) ? parseAnnotations(raw).content : raw;
        const markers = c.markers
            .filter((marker) => marker.path === rel)
            .map(({ name: label, begin, end, range }) => ({ name: label, begin, end, range }));
        return { path: name, text, markers };
    });
}

function cardValue(value: Value): CardValue {
    if (value instanceof Place) {
        return { $place: value.text, ...value.location };
    }
    if (isList(value)) {
        return value.map(cardValue);
    }
    if (isTable(value)) {
        return Object.fromEntries(
            Object.entries(value).map(([key, item]) => [key, cardValue(item)]),
        );
    }
    return value;
}

export function cardOf(c: Case, snapshot: Snapshot): Card {
    const readme = c.readme;
    if (readme === undefined) {
        throw new Error(`${c.id}: only a documented case has a card`);
    }
    const marked = new Set(c.markers.map((marker) => marker.name));
    return {
        id: c.id,
        topic: c.topic,
        subtopic: c.subtopic,
        title: readme.title,
        summary: readme.summary,
        prose: readme.prose,
        status: statusOf(writeSnapshot(snapshot)),
        issues: readme.issues,
        files: filesOf(c),
        tests: [...snapshot].map(([name, blocks]) => ({
            name,
            blocks: [...blocks].map(([label, table]) => ({
                label,
                ...(marked.has(label) ? { marker: label } : {}),
                value: cardValue(table),
            })),
        })),
    };
}

/// The card of a documented case, under CLICE_CARDS_DIR when it is set.
export function writeCard(c: Case, snapshot: Snapshot): void {
    const dir = process.env["CLICE_CARDS_DIR"];
    if (dir === undefined || c.readme === undefined) {
        return;
    }
    const file = path.join(dir, `${c.id}.json`);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, `${JSON.stringify(cardOf(c, snapshot), null, 2)}\n`);
}
