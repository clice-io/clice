/// How a case's inputs compile (project.jsonc, merged from the topic, the
/// subtopic and the case) and the copy a test runs on: the inputs with
/// their markers stripped, `*.in` files with `${WS}` replaced, and the
/// databases the project describes.

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { parse, printParseErrorCode, type ParseError } from "jsonc-parser";
import { parseAnnotations } from "../../snap/annotation.ts";

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
export const CASES_DIR = path.join(REPO_ROOT, "tests-next");

export type UnitArgs = string[] | string[][];

export interface Project {
    flags?: string[];
    /// Workspace-relative units with their own arguments; a unit of several
    /// commands lists one argument list each.
    units?: Record<string, UnitArgs>;
    /// Directories that get a database, each listing the units it holds;
    /// `{}` writes none. Without it one database at the root holds every unit.
    databases?: Record<string, string[]>;
    /// The code does not compile on purpose.
    compiles?: boolean;
}

/// Files of a case directory that are not its inputs.
export const RESERVED = new Set([
    "project.jsonc",
    "test.ts",
    "README.md",
    "snap.toml",
    "trace.jsonl",
]);

const SOURCE = /\.(c|cc|cpp|cxx|cppm|ixx)$/;
/// The files that may carry markers; any other input copies byte for byte.
export const MARKED = /\.(c|cc|cpp|cxx|cppm|ixx|h|hh|hpp|hxx)$/;
const KEYS = ["flags", "units", "databases", "compiles"];

export function posix(file: string): string {
    return file.split(path.sep).join("/");
}

function fail(where: string, message: string): never {
    throw new Error(`${where}: ${message}`);
}

function strings(where: string, key: string, value: unknown): string[] {
    if (!Array.isArray(value) || !value.every((item) => typeof item === "string")) {
        fail(where, `${key} is a list of strings`);
    }
    return value;
}

function readUnits(where: string, units: unknown): Record<string, UnitArgs> {
    if (typeof units !== "object" || units === null || Array.isArray(units)) {
        fail(where, "units maps a file to its arguments");
    }
    const parsed: Record<string, UnitArgs> = {};
    for (const [unit, args] of Object.entries(units as Record<string, unknown>)) {
        const lists = Array.isArray(args) ? (args as unknown[]) : [];
        parsed[unit] =
            lists.length > 0 && lists.every((item) => Array.isArray(item))
                ? lists.map((list) => strings(where, `units.${unit}`, list))
                : strings(where, `units.${unit}`, args);
    }
    return parsed;
}

/// The project.jsonc in `dir` (comments allowed), empty when there is none.
export function readProject(dir: string): Project {
    const file = path.join(dir, "project.jsonc");
    if (!fs.existsSync(file)) {
        return {};
    }
    const where = posix(path.relative(REPO_ROOT, file));
    const errors: ParseError[] = [];
    const raw = parse(fs.readFileSync(file, "utf8"), errors) as Record<string, unknown>;
    const error = errors[0];
    if (error !== undefined) {
        fail(where, `${printParseErrorCode(error.error)} at offset ${error.offset}`);
    }
    for (const key of Object.keys(raw)) {
        if (!KEYS.includes(key)) {
            fail(where, `unknown key '${key}'`);
        }
    }
    const project: Project = {};
    if (raw["flags"] !== undefined) {
        project.flags = strings(where, "flags", raw["flags"]);
    }
    if (raw["units"] !== undefined) {
        project.units = readUnits(where, raw["units"]);
    }
    const databases = raw["databases"];
    if (databases !== undefined) {
        if (typeof databases !== "object" || databases === null || Array.isArray(databases)) {
            fail(where, "databases maps a directory to the units it lists");
        }
        project.databases = Object.fromEntries(
            Object.entries(databases).map(([dir, units]) => [
                dir,
                strings(where, `databases.${dir}`, units),
            ]),
        );
    }
    if (raw["compiles"] !== undefined) {
        if (typeof raw["compiles"] !== "boolean") {
            fail(where, "compiles is true or false");
        }
        project.compiles = raw["compiles"];
    }
    return project;
}

/// Outer first: lists add up, the rest is replaced.
export function mergeProjects(projects: readonly Project[]): Project {
    const out: Project = {};
    for (const project of projects) {
        if (project.flags !== undefined) {
            out.flags = [...(out.flags ?? []), ...project.flags];
        }
        if (project.units !== undefined) {
            out.units = project.units;
        }
        if (project.databases !== undefined) {
            out.databases = project.databases;
        }
        if (project.compiles !== undefined) {
            out.compiles = project.compiles;
        }
    }
    return out;
}

/// The input files of a case directory, relative and sorted, as the copy
/// names them (a `*.in` without its suffix).
export function inputFiles(dir: string): string[] {
    return fs
        .readdirSync(dir, { recursive: true, encoding: "utf8" })
        .map(posix)
        .filter((rel) => !RESERVED.has(rel) && fs.statSync(path.join(dir, rel)).isFile())
        .sort();
}

export function copyName(rel: string): string {
    return rel.endsWith(".in") ? rel.slice(0, -".in".length) : rel;
}

interface Entry {
    directory: string;
    file: string;
    arguments: string[];
}

function entries(project: Project, units: Record<string, UnitArgs>, root: string): Entry[] {
    const workspace = posix(root);
    return Object.entries(units).flatMap(([unit, own]) => {
        const file = `${workspace}/${unit}`;
        const commands = Array.isArray(own[0]) ? (own as string[][]) : [own as string[]];
        return commands.map((args) => ({
            directory: workspace,
            file,
            arguments: [
                unit.endsWith(".c") ? "clang" : "clang++",
                ...(project.flags ?? []),
                ...args,
                "-fsyntax-only",
                file,
            ],
        }));
    });
}

/// Every unit of the project with its own arguments: those it lists, or
/// every C-family source of the copy.
function projectUnits(project: Project, files: readonly string[]): Record<string, UnitArgs> {
    if (project.units !== undefined) {
        return project.units;
    }
    return Object.fromEntries(files.filter((file) => SOURCE.test(file)).map((file) => [file, []]));
}

/// Write the database of `dir` under `root` listing `units`.
export function writeDatabase(
    root: string,
    project: Project,
    units: Record<string, UnitArgs>,
    dir = ".",
): void {
    const target = path.join(root, dir, "compile_commands.json");
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, `${JSON.stringify(entries(project, units, root), null, 2)}\n`);
}

/// The databases the project describes for a copy at `root`.
export function writeDatabases(project: Project, files: readonly string[], root: string): void {
    const units = projectUnits(project, files);
    for (const [dir, listed] of Object.entries(project.databases ?? { ".": Object.keys(units) })) {
        const own = listed.map((unit): [string, UnitArgs] => {
            const args = units[unit];
            if (args === undefined) {
                throw new Error(`databases.${dir} lists ${unit}, which is not a unit`);
            }
            return [unit, args];
        });
        writeDatabase(root, project, Object.fromEntries(own), dir);
    }
}

/// Copy the inputs of `dir` to `root`: markers stripped, `*.in` with
/// `${WS}` replaced, any other file byte for byte. Returns the copy's file
/// names.
export function materialize(dir: string, root: string): string[] {
    const workspace = posix(root);
    const files: string[] = [];
    for (const rel of inputFiles(dir)) {
        const name = copyName(rel);
        const target = path.join(root, name);
        fs.mkdirSync(path.dirname(target), { recursive: true });
        const bytes = fs.readFileSync(path.join(dir, rel));
        if (rel.endsWith(".in")) {
            fs.writeFileSync(target, bytes.toString("utf8").replaceAll("${WS}", workspace));
        } else if (MARKED.test(rel) && bytes.includes("§")) {
            fs.writeFileSync(target, parseAnnotations(bytes.toString("utf8")).content);
        } else {
            fs.writeFileSync(target, bytes);
        }
        files.push(name);
    }
    return files;
}
