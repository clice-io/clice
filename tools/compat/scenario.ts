/// Compatibility scenarios: one real build system driving one real
/// toolchain over the shared project in tests/compat/project. A scenario
/// builds a throwaway copy of the project, so the compilation database it
/// is checked against is what the tools write today, never a stored copy.

import * as fs from "node:fs";
import * as path from "node:path";

/// What clice must make of one source file's compile command, beyond the
/// checks every file gets (a database entry, a resolved toolchain, the
/// real compiler's macro values, a clean parse).
export interface FileExpectation {
    /// Argument sequences the database entry itself must contain: the
    /// shape the scenario exists to exercise, so a tool release that stops
    /// writing it fails the scenario instead of quietly testing less.
    recorded?: string[][];

    /// Argument sequences the resolved command must contain, each adjacent
    /// and in order; `${root}` stands for the project copy.
    contains?: string[][];

    /// Arguments the resolved command must not contain.
    excludes?: string[];
}

export interface Scenario {
    /// What the scenario exercises, e.g. "cmake ninja gcc".
    name: string;

    /// The platforms whose system toolchain the scenario names.
    platforms: NodeJS.Platform[];

    /// Executables the scenario runs, by name or absolute path.
    requires: string[];

    /// Build steps run in order in the project copy, each an argv.
    build: [string, ...string[]][];

    /// The project's source files under check, project-relative.
    files: Record<string, FileExpectation>;
}

/// The environment builds and clice run in: the caller's, minus pixi
/// environments, whose compilers and build tools would otherwise shadow
/// the system toolchain a scenario names by bare name (meson and bear
/// write `cc`, `x86_64-w64-mingw32-g++` as found on PATH).
export function systemEnv(): NodeJS.ProcessEnv {
    const env = { ...process.env };
    const entries = (env["PATH"] ?? "").split(path.delimiter);
    env["PATH"] = entries
        .filter((entry) => !entry.includes(`.pixi${path.sep}envs`))
        .join(path.delimiter);
    return env;
}

function locate(tool: string): string | undefined {
    if (path.isAbsolute(tool)) {
        return fs.existsSync(tool) ? tool : undefined;
    }
    for (const dir of (systemEnv()["PATH"] ?? "").split(path.delimiter)) {
        const candidate = path.join(dir, tool);
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

export function missingTools(scenario: Scenario): string[] {
    return scenario.requires.filter((tool) => locate(tool) === undefined);
}
