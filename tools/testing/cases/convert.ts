/// Rewrites the JSON configuration of a branch from before project.toml:
/// every tracked `tests/**/project.jsonc` (or `project.json`) becomes
/// `project.toml`, every `ecosystem/*/project.json` `project.toml` with its
/// steps as `[[prepare]]` tables, and `tests/coverage.json`
/// `tests/coverage.toml`. `node tools/testing/cases/convert.ts`, then
/// `pixi run format`. Each comment stays above the key it preceded, and
/// each file is read back and compared with what it was before the JSON is
/// deleted. It goes, with the readers' refusal of the JSON files, once no
/// branch has them.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { isDeepStrictEqual } from "node:util";
import { parse as parseJsonc, visit, type ParseError } from "jsonc-parser";
import { parse as parseToml } from "smol-toml";
import { REPO_ROOT } from "../../repo.ts";

type Json = string | number | boolean | null | Json[] | { [key: string]: Json };
type Table = Record<string, Json>;

function isTable(value: Json | undefined): value is Table {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

function nonEmptyTable(value: Json): value is Table {
    return isTable(value) && Object.keys(value).length > 0;
}

function isTableList(value: Json): value is Table[] {
    return Array.isArray(value) && value.length > 0 && value.every(isTable);
}

function tomlKey(name: string): string {
    return /^[\w-]+$/.test(name) ? name : JSON.stringify(name);
}

function tomlValue(value: Json): string {
    if (typeof value === "string") {
        const literal = /[\\"]/.test(value) && !/['\n\r\t]/.test(value);
        return literal ? `'${value}'` : JSON.stringify(value);
    }
    if (typeof value === "number" || typeof value === "boolean") {
        return String(value);
    }
    if (Array.isArray(value)) {
        return `[${value.map(tomlValue).join(", ")}]`;
    }
    if (value === null) {
        throw new Error("TOML has no null");
    }
    const entries = Object.entries(value).map(([k, v]) => `${tomlKey(k)} = ${tomlValue(v)}`);
    return entries.length === 0 ? "{}" : `{ ${entries.join(", ")} }`;
}

/// The comments of a JSONC text by the key path of the property each one
/// precedes, the keys joined by `\0`.
function commentsOf(text: string): Map<string, string[]> {
    const properties: { offset: number; where: string }[] = [];
    const found: { offset: number; text: string }[] = [];
    visit(text, {
        onObjectProperty(property, offset, _length, _line, _column, pathSupplier) {
            properties.push({ offset, where: [...pathSupplier(), property].join("\0") });
        },
        onComment(offset, length) {
            found.push({ offset, text: text.slice(offset, offset + length) });
        },
    });
    const comments = new Map<string, string[]>();
    for (const comment of found) {
        const next = properties.find((property) => property.offset > comment.offset);
        const between =
            next === undefined ? "" : text.slice(comment.offset + comment.text.length, next.offset);
        if (
            next === undefined ||
            !comment.text.startsWith("//") ||
            !/^\s*(\/\/[^\n]*\s*)*$/.test(between)
        ) {
            throw new Error(`the comment ${comment.text} is not a line right above a key`);
        }
        const line = comment.text.slice(2).replace(/^ /, "").trimEnd();
        comments.set(next.where, [...(comments.get(next.where) ?? []), line]);
    }
    return comments;
}

function toToml(document: Table, comments: Map<string, string[]>): string {
    const out: string[] = [];
    const placed = new Set<string>();
    const comment = (where: string[]): void => {
        const key = where.join("\0");
        placed.add(key);
        out.push(...(comments.get(key) ?? []).map((line) => (line === "" ? "#" : `# ${line}`)));
    };
    const table = (value: Table, where: string[], header: string[], element: boolean): void => {
        const entries = Object.entries(value);
        const inline = entries.filter(([, v]) => !nonEmptyTable(v) && !isTableList(v));
        const nested = entries.filter(([, v]) => nonEmptyTable(v) || isTableList(v));
        if (header.length > 0 && (element || inline.length > 0 || nested.length === 0)) {
            const last = out.at(-1);
            if (last !== undefined && last !== "" && !last.startsWith("[")) {
                out.push("");
            }
            comment(where);
            out.push(element ? `[[${header.join(".")}]]` : `[${header.join(".")}]`);
        }
        for (const [key, item] of inline) {
            comment([...where, key]);
            out.push(`${tomlKey(key)} = ${tomlValue(item)}`);
        }
        for (const [key, item] of nested) {
            if (isTableList(item)) {
                item.forEach((each, index) => {
                    table(each, [...where, key, String(index)], [...header, tomlKey(key)], true);
                });
            } else if (isTable(item)) {
                table(item, [...where, key], [...header, tomlKey(key)], false);
            }
        }
    };
    table(document, [], [], false);
    const lost = [...comments.keys()].filter((key) => !placed.has(key));
    if (lost.length > 0) {
        throw new Error(`comments above ${lost.join(", ").replaceAll("\0", ".")} have no place`);
    }
    return `${out.join("\n")}\n`;
}

function convert(from: string, to: string, transform: (document: Table) => Table): void {
    const where = path.relative(REPO_ROOT, from);
    if (fs.existsSync(to)) {
        throw new Error(`${where}: ${path.relative(REPO_ROOT, to)} exists already`);
    }
    const text = fs.readFileSync(from, "utf8");
    const errors: ParseError[] = [];
    const parsed = parseJsonc(text, errors, { allowTrailingComma: true }) as Json;
    if (errors.length > 0 || !isTable(parsed)) {
        throw new Error(`${where}: not a JSON object`);
    }
    const document = transform(parsed);
    const toml = toToml(document, commentsOf(text));
    if (!isDeepStrictEqual(JSON.parse(JSON.stringify(parseToml(toml))), document)) {
        throw new Error(`${where}: its TOML reads back as something else`);
    }
    fs.writeFileSync(to, toml);
    fs.rmSync(from);
    console.log(`${where} -> ${path.relative(REPO_ROOT, to)}`);
}

/// An argv step is `{ run = [...] }`.
function prepareTables(project: Table): Table {
    const configurations = project["configurations"];
    for (const config of isTable(configurations) ? Object.values(configurations) : []) {
        const prepare = isTable(config) ? config["prepare"] : undefined;
        if (isTable(config) && Array.isArray(prepare)) {
            config["prepare"] = prepare.map((step) => (Array.isArray(step) ? { run: step } : step));
        }
    }
    return project;
}

const tracked = execFileSync(
    "git",
    ["ls-files", "tests/**/project.jsonc", "tests/**/project.json", "ecosystem/*/project.json"],
    { cwd: REPO_ROOT, encoding: "utf8" },
)
    .split("\n")
    .filter((file) => file !== "");
for (const file of tracked) {
    const from = path.join(REPO_ROOT, file);
    const to = path.join(path.dirname(from), "project.toml");
    convert(from, to, file.startsWith("ecosystem/") ? prepareTables : (document) => document);
}
const coverage = path.join(REPO_ROOT, "tests", "coverage.json");
if (fs.existsSync(coverage)) {
    convert(coverage, path.join(REPO_ROOT, "tests", "coverage.toml"), (document) => document);
}
