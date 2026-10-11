/// A documented case as the docs site draws it: its page, its sources with
/// the markers taken out and their offsets kept, and its output as the run
/// left it, every place with the range it came from, so the site draws
/// answers into the code without reading snap.toml's text. Every run checks
/// a documented case's card against `docs/cards/<id>.json`, which the docs
/// sync hands to the site; UPDATE_SNAPSHOTS=1 rewrites it.

import * as fs from "node:fs";
import * as path from "node:path";
import { parseAnnotations } from "../../snap/annotation.ts";
import type { Case } from "../cases/case.ts";
import { statusOf, writeSnapshot, type Snapshot } from "../cases/output.ts";
import { MARKED, REPO_ROOT, copyName } from "../cases/project.ts";
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

export const CARDS_DIR = path.join(REPO_ROOT, "docs", "cards");

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
    /// Each test with the options it set, shaped as clice.toml.
    tests: { name: string; config?: CardValue; blocks: CardBlock[] }[];
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

/// The configuration each test of the case sets, by test name.
export type Configs = ReadonlyMap<string, Record<string, unknown>>;

export function cardOf(c: Case, snapshot: Snapshot, configs: Configs): Card {
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
        tests: [...snapshot].map(([name, blocks]) => {
            const config = configs.get(name);
            return {
                name,
                ...(config === undefined ? {} : { config: config as CardValue }),
                blocks: [...blocks].map(([label, table]) => ({
                    label,
                    ...(marked.has(label) ? { marker: label } : {}),
                    value: cardValue(table),
                })),
            };
        }),
    };
}

export function cardPath(id: string): string {
    return path.join(CARDS_DIR, `${id}.json`);
}

/// A documented case's card against its file: UPDATE_SNAPSHOTS=1 rewrites
/// it, a run otherwise fails where they differ.
export function checkCard(c: Case, snapshot: Snapshot, configs: Configs): void {
    if (c.readme === undefined) {
        return;
    }
    const file = cardPath(c.id);
    const text = `${JSON.stringify(cardOf(c, snapshot, configs), null, 2)}\n`;
    if (fs.existsSync(file) && fs.readFileSync(file, "utf8") === text) {
        return;
    }
    if (process.env["UPDATE_SNAPSHOTS"] !== "1") {
        throw new Error(
            `${c.id}: its card differs from ${path.relative(REPO_ROOT, file)} (UPDATE_SNAPSHOTS=1 rewrites it)`,
        );
    }
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text);
}
