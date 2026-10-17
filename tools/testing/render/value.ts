/// What an output holds and how snap.toml writes it: text, numbers, flags,
/// places, lists and tables. Short values stay on a line; text spanning
/// lines is a raw ''' string, so code, markdown and diffs read as they are,
/// unescaped. The layout is the one tombi gives TOML 1.1 under the
/// repository's rules (tombi.toml), so formatting leaves snap.toml as written.

import { parse } from "smol-toml";
import type { Location } from "./position.ts";

/// A place an answer names, written as its rendering (`int ⟦x⟧ = 0;`);
/// the range it came from stays with it for the docs.
export class Place {
    readonly text: string;
    readonly location: Location;

    constructor(text: string, location: Location) {
        this.text = text;
        this.location = location;
    }
}

export type Value = string | number | boolean | Place | readonly Value[] | Table;

export interface Table {
    readonly [key: string]: Value;
}

export function isList(value: Value): value is readonly Value[] {
    return Array.isArray(value);
}

export function isTable(value: Value): value is Table {
    return typeof value === "object" && !isList(value) && !(value instanceof Place);
}

/// `value` with its places replaced by their text: what a reader of
/// snap.toml gets.
export function plain(value: Value): Value {
    if (value instanceof Place) {
        return value.text;
    }
    if (isList(value)) {
        return value.map(plain);
    }
    if (isTable(value)) {
        return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, plain(item)]));
    }
    return value;
}

/// Every string of `value`, places' text included, through `map`.
export function mapText(value: Value, map: (text: string) => string): Value {
    if (typeof value === "string") {
        return map(value);
    }
    if (value instanceof Place) {
        return new Place(map(value.text), value.location);
    }
    if (isList(value)) {
        return value.map((item) => mapText(item, map));
    }
    if (isTable(value)) {
        return Object.fromEntries(
            Object.entries(value).map(([key, item]) => [key, mapText(item, map)]),
        );
    }
    return value;
}

const WIDTH = 120;
const INDENT = "    ";
const GRAPHEMES = new Intl.Segmenter(undefined, { granularity: "grapheme" });
const BARE = /^[A-Za-z0-9_-]+$/;
/// What a literal string cannot hold: control characters but the tab.
const CONTROL = /[\u0000-\u0008\u000a-\u001f\u007f]/;

function basic(text: string): string {
    let out = "";
    for (const char of text) {
        const code = char.codePointAt(0) ?? 0;
        if (char === '"' || char === "\\") {
            out += `\\${char}`;
        } else if (char === "\n") {
            out += "\\n";
        } else if (char === "\t") {
            out += "\\t";
        } else if (code < 0x20 || code === 0x7f) {
            out += `\\u${code.toString(16).padStart(4, "0")}`;
        } else {
            out += char;
        }
    }
    return `"${out}"`;
}

export function tomlKey(name: string): string {
    return BARE.test(name) ? name : basic(name);
}

/// A one-line string: raw between single quotes when a basic string would
/// escape something and the text has no quote of its own.
function line(text: string): string {
    return /["\\]/.test(text) && !text.includes("'") && !CONTROL.test(text)
        ? `'${text}'`
        : basic(text);
}

/// Text of several lines, raw between ''' when it can be; a trailing line
/// break is not kept.
function text(value: string): string {
    const body = value.replace(/\n+$/, "");
    if (!body.includes("\n")) {
        return line(body);
    }
    const raw = body.replaceAll("\n", "");
    return body.includes("'''") || CONTROL.test(raw) ? basic(body) : `'''\n${body}\n'''`;
}

function scalar(value: number | boolean): string {
    if (typeof value === "number" && !Number.isInteger(value)) {
        throw new Error(`snap.toml holds whole numbers only, not ${String(value)}`);
    }
    return String(value);
}

function scalarLike(value: Value): boolean {
    return isList(value) ? value.length === 0 : !isTable(value) || Object.keys(value).length === 0;
}

/// Columns as tombi counts them: one a grapheme.
function columns(text: string): number {
    return [...GRAPHEMES.segment(text)].length;
}

function oneLine(value: Value): string {
    if (isList(value)) {
        return `[${value.map(oneLine).join(", ")}]`;
    }
    if (!isTable(value)) {
        return inline(value, "", 0, false);
    }
    const entries = Object.entries(value);
    return entries.length === 0
        ? "{}"
        : `{ ${entries.map(([key, item]) => `${tomlKey(key)} = ${oneLine(item)}`).join(", ")} }`;
}

/// Whether tombi keeps `value` on one line, `start` columns into it: the
/// value and each list or table in it fit from `start`, every item counted
/// `extra` columns wider. Tombi measures a row of a list taking a line per
/// item from the line's start, and each item in the row as wide again as
/// the row's indentation. It measures an empty table as `{  }`, so a list
/// ending in one needs a column more.
function fits(value: Value, start: number, extra: number): boolean {
    if (!isList(value) && !isTable(value)) {
        return true;
    }
    const items = isList(value) ? value : Object.values(value);
    const last = items.at(-1);
    const tail = isList(value) && last !== undefined && isTable(last) && scalarLike(last) ? 1 : 0;
    return (
        start + columns(oneLine(value)) + extra * items.length + tail <= WIDTH &&
        items.every((item) => fits(item, start, extra))
    );
}

/// `value` written inline under `indent`, `start` columns into its line;
/// a `row` is an item of a list taking a line per item. A list or table
/// stays on one line while tombi keeps it there; a list of tables or
/// lists, and anything holding text of several lines, takes a line per
/// item. An item on a line of its own ends with a comma: tombi keeps such
/// a list or table as it stands.
function inline(value: Value, indent: string, start: number, row: boolean): string {
    if (typeof value === "string") {
        return text(value);
    }
    if (value instanceof Place) {
        return line(value.text);
    }
    if (typeof value === "number" || typeof value === "boolean") {
        return scalar(value);
    }
    if (scalarLike(value)) {
        return isList(value) ? "[]" : "{}";
    }
    const inner = indent + INDENT;
    const items = isList(value)
        ? value.map((item) => inline(item, inner, 0, true))
        : Object.entries(value).map(([key, item]) => {
              const head = `${tomlKey(key)} = `;
              return head + inline(item, inner, columns(inner + head), false);
          });
    const rows =
        items.some((item) => item.includes("\n")) ||
        (isList(value) && value.some((item) => !scalarLike(item)));
    if (!rows && fits(value, start, row ? indent.length : 0)) {
        return oneLine(value);
    }
    const [open, close] = isList(value) ? ["[", "]"] : ["{", "}"];
    return `${open}\n${items.map((item) => `${inner}${item},`).join("\n")}\n${indent}${close}`;
}

/// Whether `value` holds text of several lines.
function spans(value: Value): boolean {
    if (typeof value === "string") {
        return value.replace(/\n+$/, "").includes("\n");
    }
    if (isList(value)) {
        return value.some(spans);
    }
    return isTable(value) && Object.values(value).some(spans);
}

/// The sections of a table: its header and each entry `key = value`; an
/// entry that is a table follows as a section of its own, `[path.key]`, and
/// a list of tables holding text of several lines as one section an item,
/// `[[path.key]]`. A header with nothing under it but sections goes
/// without saying, but for an item's, which sits right above its first
/// section, as tombi sets it.
function sections(path: readonly string[], table: Table, item = false): string[][] {
    const keys = path.map(tomlKey).join(".");
    const header = item ? `[[${keys}]]` : `[${keys}]`;
    const entries = Object.entries(table);
    const nested = entries.filter(([, value]) => isTable(value) && !scalarLike(value));
    const rows = entries.filter(
        ([, value]) =>
            isList(value) && value.every((row) => !scalarLike(row) && isTable(row)) && spans(value),
    );
    const body = entries
        .filter((entry) => !nested.includes(entry) && !rows.includes(entry))
        .map(([key, value]) => {
            const head = `${tomlKey(key)} = `;
            return head + inline(value, "", columns(head), false);
        });
    const children = [
        ...nested.flatMap(([key, value]) => sections([...path, key], value as Table)),
        ...rows.flatMap(([key, value]) =>
            (value as readonly Table[]).flatMap((row) => sections([...path, key], row, true)),
        ),
    ];
    const [first, ...rest] = children;
    if (body.length > 0 || first === undefined) {
        return [[header, ...body], ...children];
    }
    return item ? [[header, ...first], ...rest] : children;
}

/// A file of tables, `[path…]` each; empty for none. Read back, it must
/// give the same values: a writer bug fails here, never in a reader.
export function writeToml(tables: readonly { path: readonly string[]; table: Table }[]): string {
    if (tables.length === 0) {
        return "";
    }
    const text = `${tables
        .flatMap(({ path, table }) => sections(path, table).map((lines) => lines.join("\n")))
        .join("\n\n")}\n`;
    const back = readToml(text);
    for (const { path, table } of tables) {
        let read: unknown = back;
        for (const key of path) {
            read = (read as Record<string, unknown> | undefined)?.[key];
        }
        if (canonical(read) !== canonical(trimmed(plain(table)))) {
            throw new Error(`snap.toml reads [${path.join(".")}] back differently:\n${text}`);
        }
    }
    return text;
}

/// `value` as JSON with its keys sorted: TOML writes a table's plain
/// entries before its tables, so only the order of keys may differ.
function canonical(value: unknown): string {
    return JSON.stringify(value, (_key, item: unknown) =>
        typeof item === "object" && item !== null && !Array.isArray(item)
            ? Object.fromEntries(Object.entries(item).sort(([a], [b]) => (a < b ? -1 : 1)))
            : item,
    );
}

/// `value` as it reads back: text without a trailing line break.
function trimmed(value: Value): Value {
    return mapText(value, (text) => text.replace(/\n+$/, ""));
}

/// The tables of a snap.toml, its text without trailing line breaks.
export function readToml(text: string): Record<string, unknown> {
    return trimmed(parse(text) as Table) as Record<string, unknown>;
}
