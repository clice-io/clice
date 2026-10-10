/// Sample projects (samples/): hand-written workspaces the serve
/// scenarios run on. Each carries a manifest, project.json, naming its
/// translation units with their arguments and the logical names its files
/// go by; a case copies the project and writes the compilation database
/// the manifest describes, with the copy's real root and the test
/// toolchain's compilers.

import * as fs from "node:fs";
import * as path from "node:path";
import { posix, REPO_ROOT } from "../compile_commands.ts";
import type { Workspace } from "./workspace.ts";

export const SAMPLES_DIR = path.join(REPO_ROOT, "samples");

export interface Manifest {
    /// Arguments of every unit; then those of its language, then its own.
    args?: string[];
    cxx?: string[];
    c?: string[];
    /// The translation units, workspace-relative, with their own arguments;
    /// a unit of several configurations has one entry per argument list.
    /// `${workspace}` in an argument stands for the workspace root. None for
    /// a sample that ships its own databases (samples/layouts).
    units?: Record<string, string[] | string[][]>;
    /// Logical names of files: what a scenario calls a file whatever the
    /// variant spells it ("circle" is a header in one, a module in another).
    files?: Record<string, string>;
}

const SOURCE = /\.(c|cc|cpp|cxx|cppm|ixx)$/;

export function readManifest(project: string): Manifest {
    return JSON.parse(
        fs.readFileSync(path.join(SAMPLES_DIR, project, "project.json"), "utf8"),
    ) as Manifest;
}

/// The manifest of loose files: every source a unit with the default
/// arguments.
export function looseManifest(files: Iterable<string>): Manifest {
    const units: Record<string, string[]> = {};
    for (const file of files) {
        if (SOURCE.test(file)) {
            units[file] = [];
        }
    }
    return { cxx: ["-std=c++23"], c: ["-std=c17"], units };
}

/// Write the compilation database `manifest` describes at `at` under
/// `workspace`, its entries' directory the root.
export function writeDatabase(
    workspace: Workspace,
    manifest: Manifest,
    at = "compile_commands.json",
): void {
    const root = posix(workspace.root);
    const entries = Object.entries(manifest.units ?? {}).flatMap(([unit, own]) => {
        const c = unit.endsWith(".c");
        const file = posix(workspace.path(unit));
        const configurations = Array.isArray(own[0]) ? (own as string[][]) : [own as string[]];
        return configurations.map((args) => ({
            directory: root,
            file,
            arguments: [
                c ? "clang" : "clang++",
                ...(manifest.args ?? []),
                ...((c ? manifest.c : manifest.cxx) ?? []),
                ...args,
                "-fsyntax-only",
                file,
            ].map((arg) => arg.replaceAll("${workspace}", root)),
        }));
    });
    workspace.write(at, JSON.stringify(entries, null, 2));
}

/// Copy `project` into `workspace`; returns its manifest.
export function materialize(project: string, workspace: Workspace): Manifest {
    fs.cpSync(path.join(SAMPLES_DIR, project), workspace.root, { recursive: true });
    return readManifest(project);
}
