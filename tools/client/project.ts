/// Sample projects (tests/projects): hand-written workspaces the serve
/// scenarios run on. Each carries a manifest, project.json, naming its
/// translation units with their arguments and the logical names its files
/// go by; a case copies the project and writes the compilation database
/// the manifest describes, with the copy's real root and the test
/// toolchain's compilers.

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";
import type { Workspace } from "./workspace.ts";

export const PROJECTS_DIR = path.join(REPO_ROOT, "tests", "projects");

export interface Manifest {
    /// Arguments of every unit; then those of its language, then its own.
    args?: string[];
    cxx?: string[];
    c?: string[];
    /// The translation units, workspace-relative, with their own arguments.
    units: Record<string, string[]>;
    /// Logical names of files: what a scenario calls a file whatever the
    /// variant spells it ("circle" is a header in one, a module in another).
    files?: Record<string, string>;
}

const SOURCE = /\.(c|cc|cpp|cxx|cppm|ixx)$/;

export function readManifest(project: string): Manifest {
    return JSON.parse(
        fs.readFileSync(path.join(PROJECTS_DIR, project, "project.json"), "utf8"),
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

function posix(p: string): string {
    return p.split(path.sep).join("/");
}

/// Write the compilation database `manifest` describes at the root of
/// `workspace`.
export function writeDatabase(workspace: Workspace, manifest: Manifest): void {
    const entries = Object.entries(manifest.units).map(([unit, own]) => {
        const c = unit.endsWith(".c");
        const file = posix(workspace.path(unit));
        return {
            directory: posix(workspace.root),
            file,
            arguments: [
                c ? "clang" : "clang++",
                ...(manifest.args ?? []),
                ...((c ? manifest.c : manifest.cxx) ?? []),
                ...own,
                "-fsyntax-only",
                file,
            ],
        };
    });
    workspace.write("compile_commands.json", JSON.stringify(entries, null, 2));
}

/// Copy `project` into `workspace` and write its database; returns its
/// manifest.
export function materialize(project: string, workspace: Workspace): Manifest {
    fs.cpSync(path.join(PROJECTS_DIR, project), workspace.root, { recursive: true });
    const manifest = readManifest(project);
    writeDatabase(workspace, manifest);
    return manifest;
}
