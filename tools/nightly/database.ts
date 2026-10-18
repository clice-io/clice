/// A job's compilation database: the project's own, or the entries of the
/// job's part written beside it with a clice.toml that names them, so clice
/// loads that one database and no other the tree happens to hold.

import * as fs from "node:fs";
import * as path from "node:path";
import { entryArguments, entrySource, type DatabaseEntry } from "../compat/database.ts";
import type { Job } from "./projects.ts";

/// Where a job's own database goes, checkout-relative.
const JOB_DATABASE = ".nightly/compile_commands.json";

export function readEntries(file: string): DatabaseEntry[] {
    return JSON.parse(fs.readFileSync(file, "utf8")) as DatabaseEntry[];
}

/// The part whose prefixes hold `source`; every unit is in exactly one.
function partOf(parts: Record<string, string[]>, checkout: string, source: string): string {
    const relative = path.relative(checkout, source).split(path.sep).join("/");
    const holding = Object.entries(parts)
        .filter(([, prefixes]) => prefixes.some((prefix) => relative.startsWith(prefix)))
        .map(([name]) => name);
    const [only] = holding;
    if (holding.length !== 1 || only === undefined) {
        throw new Error(
            `${relative} is in ${holding.length === 0 ? "no part" : `the parts ${holding.join(", ")}`}: the parts must divide the database`,
        );
    }
    return only;
}

/// The job's units, and the database clice reads them from in place.
export function jobDatabase(job: Job, checkout: string): DatabaseEntry[] {
    const entries = readEntries(path.join(checkout, job.project.database));
    const { parts } = job.project;
    if (parts === undefined && job.project.database === "compile_commands.json") {
        return entries;
    }
    const own =
        parts === undefined
            ? entries
            : entries.filter((entry) => partOf(parts, checkout, entrySource(entry)) === job.part);
    fs.mkdirSync(path.join(checkout, path.dirname(JOB_DATABASE)), { recursive: true });
    fs.writeFileSync(path.join(checkout, JOB_DATABASE), JSON.stringify(own, null, 1));
    fs.writeFileSync(
        path.join(checkout, "clice.toml"),
        `[[rules]]\ncompile_commands = ["${JOB_DATABASE}"]\n`,
    );
    return own;
}

/// Arguments that write files beside a compile: the object and the
/// dependency files.
const WRITES_WITH_VALUE = new Set(["-o", "-MF", "-MT", "-MQ"]);
const WRITES = new Set(["-c", "-MD", "-MMD"]);

/// The entry's command checking the code only: what the build ran, without
/// the files it writes, and `-fsyntax-only`. A CMake precompiled header is
/// read from its source instead (the `-include` beside it stays): the
/// preparation builds no `.pch`.
export function syntaxOnly(entry: DatabaseEntry): string[] {
    const args = entryArguments(entry);
    const out: string[] = [];
    for (let i = 0; i < args.length; i += 1) {
        const arg = args[i] ?? "";
        if (WRITES_WITH_VALUE.has(arg)) {
            i += 1;
        } else if (
            arg === "-Xclang" &&
            args[i + 1] === "-include-pch" &&
            args[i + 2] === "-Xclang"
        ) {
            i += 3;
        } else if (
            !WRITES.has(arg) &&
            !arg.startsWith("-Wp,-MD,") &&
            !arg.startsWith("-Wp,-MMD,")
        ) {
            out.push(arg);
        }
    }
    out.push("-fsyntax-only");
    return out;
}
