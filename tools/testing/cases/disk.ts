/// The disk of a test's copy, `t.disk`: every change is a step, and a
/// clice.toml written is recorded for the coverage check.

import * as fs from "node:fs";
import * as path from "node:path";
import { recordToml } from "../coverage/record.ts";
import type { StepRecord } from "../server/steps.ts";
import { applyChange, type Change } from "./anchors.ts";
import { posix, writeDatabase, writeDatabases, type Project, type UnitArgs } from "./project.ts";

export function describeChange(change: Change): string {
    if ("text" in change) {
        return "replace the text";
    }
    if ("replace" in change) {
        return `replace ${JSON.stringify(change.replace)} with ${JSON.stringify(change.with)}`;
    }
    if ("after" in change) {
        return `insert ${JSON.stringify(change.insert)} after ${JSON.stringify(change.after)}`;
    }
    if ("before" in change) {
        return `insert ${JSON.stringify(change.insert)} before ${JSON.stringify(change.before)}`;
    }
    return `remove ${JSON.stringify(change.remove)}`;
}

/// What a damaged stretch reads. Whether clang's reader crashes on damage
/// past its checks depends on the bytes it hits, so a test's server crashes
/// on a PCH holding this instead (CLICE_TEST_DAMAGE).
export const DAMAGE = "clice test: damaged past the checks";

export class Disk {
    private readonly root: string;
    private readonly project: Project;
    private readonly steps: StepRecord;
    /// Whether a database was written: the first server or command writes
    /// the project's unless the test wrote one.
    written = false;

    constructor(root: string, project: Project, steps: StepRecord) {
        this.root = root;
        this.project = project;
        this.steps = steps;
    }

    private path(file: string): string {
        return path.join(this.root, file);
    }

    read(file: string): string {
        return fs.readFileSync(this.path(file), "utf8");
    }

    /// Write one file, or several (generated inputs).
    write(file: string, text: string): void;
    write(files: Record<string, string>): void;
    write(fileOrFiles: string | Record<string, string>, text = ""): void {
        const files = typeof fileOrFiles === "string" ? { [fileOrFiles]: text } : fileOrFiles;
        const names = Object.keys(files);
        this.steps.note(
            names.length === 1
                ? `disk: write ${names[0] ?? ""}`
                : `disk: write ${names.length} files`,
        );
        for (const [file, content] of Object.entries(files)) {
            const target = this.path(file);
            fs.mkdirSync(path.dirname(target), { recursive: true });
            fs.writeFileSync(target, content);
            if (path.basename(file) === "clice.toml") {
                recordToml(content);
            }
        }
    }

    edit(file: string, ...changes: Change[]): void {
        this.steps.note(`disk: edit ${file}: ${changes.map(describeChange).join("; ")}`);
        let text = this.read(file);
        for (const change of changes) {
            text = applyChange(text, change, file);
        }
        fs.writeFileSync(this.path(file), text);
        if (path.basename(file) === "clice.toml") {
            recordToml(text);
        }
    }

    /// Remove a file or a directory with what it holds.
    rm(file: string): void {
        this.steps.note(`disk: remove ${file}`);
        fs.rmSync(this.path(file), { recursive: true });
    }

    /// Move a file or a directory with what it holds.
    move(from: string, to: string): void {
        this.steps.note(`disk: move ${from} to ${to}`);
        fs.mkdirSync(path.dirname(this.path(to)), { recursive: true });
        fs.renameSync(this.path(from), this.path(to));
    }

    /// Cut a file short at `bytes`, as a crash mid-write leaves it.
    truncate(file: string, bytes: number): void {
        this.steps.note(`disk: truncate ${file} to ${String(bytes)} bytes`);
        fs.truncateSync(this.path(file), bytes);
    }

    symlink(link: string, target: string): void {
        this.steps.note(`disk: link ${link} to ${target}`);
        fs.mkdirSync(path.dirname(this.path(link)), { recursive: true });
        fs.symlinkSync(path.isAbsolute(target) ? target : this.path(target), this.path(link));
    }

    /// Point a directory link at `target`, replacing the one there: a
    /// junction on Windows, which needs no privilege, a symlink elsewhere.
    linkDirectory(link: string, target: string): void {
        this.steps.note(`disk: link directory ${link} to ${target}`);
        const at = this.path(link);
        if (fs.lstatSync(at, { throwIfNoEntry: false })) {
            fs.unlinkSync(at);
        }
        fs.mkdirSync(path.dirname(at), { recursive: true });
        fs.symlinkSync(this.path(target), at, "junction");
    }

    mtime(file: string): Date {
        return fs.statSync(this.path(file)).mtime;
    }

    /// Set a file's modification time, as a tool restoring it does.
    touch(file: string, mtime: Date): void {
        this.steps.note(`disk: touch ${file} at ${mtime.toISOString()}`);
        fs.utimesSync(this.path(file), mtime, mtime);
    }

    /// The files matching `pattern`, copy-relative and sorted.
    list(pattern: string): string[] {
        return fs
            .globSync(pattern, { cwd: this.root })
            .map(posix)
            .filter((file) => fs.statSync(this.path(file)).isFile())
            .sort();
    }

    /// Damage a file as a crash or a bad disk would, its size kept and its
    /// time not moved forward: every byte, a stretch in its middle that
    /// reads `DAMAGE`, or what `where` changes in its bytes.
    corrupt(file: string, where: "all" | "middle" | ((bytes: Buffer) => void)): void {
        const what = where === "all" ? "all of" : where === "middle" ? "the middle of" : "part of";
        this.steps.note(`disk: corrupt ${what} ${file}`);
        const target = this.path(file);
        const { atime, mtimeMs } = fs.statSync(target);
        const bytes = fs.readFileSync(target);
        if (where === "all") {
            for (let i = 0; i < bytes.length; i += 1) {
                bytes[i] = (bytes[i] ?? 0) ^ 0xff;
            }
        } else if (where === "middle") {
            bytes.write(DAMAGE, Math.max(0, Math.floor((bytes.length - DAMAGE.length) / 2)));
        } else {
            where(bytes);
        }
        fs.writeFileSync(target, bytes);
        // stat's Date rounds to the nearest millisecond, which can move the
        // time past a file written just after this one: the cache store then
        // drops a PCH's index as older than the PCH. A millisecond back stays
        // behind it.
        fs.utimesSync(target, atime, new Date(Math.floor(mtimeMs) - 1));
    }

    /// Write a database: the project's for the copy as it stands, or that of
    /// `dir` listing `units` with the project's flags and their own.
    database(units?: Record<string, UnitArgs>, dir = "."): void {
        this.written = true;
        if (units === undefined) {
            this.steps.note("disk: the project's databases");
            writeDatabases(this.project, this.files(), this.root);
            return;
        }
        const names = Object.keys(units).join(", ");
        this.steps.note(
            `disk: ${dir}/compile_commands.json of ${names === "" ? "no unit" : names}`,
        );
        writeDatabase(this.root, this.project, units, dir);
    }

    /// The copy's files, the server's cache left out.
    private files(): string[] {
        return fs
            .readdirSync(this.root, { recursive: true, encoding: "utf8" })
            .map(posix)
            .filter(
                (file) =>
                    !file.split("/").includes(".clice") &&
                    fs.statSync(this.path(file), { throwIfNoEntry: false })?.isFile() === true,
            )
            .sort();
    }
}
