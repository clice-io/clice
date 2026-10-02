/// Compatibility scenarios: one real build system driving one real
/// toolchain over the shared project in tests/compat/project. A scenario
/// builds a throwaway copy of the project, so the compilation database it
/// is checked against is what the tools write today, never a stored copy.

import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { REPO_ROOT } from "../compile_commands.ts";

export const PROJECT_DIR = path.join(REPO_ROOT, "tests", "compat", "project");

/// What clice must make of one source file's compile command, beyond the
/// checks every file gets (a database entry, a resolved toolchain, the
/// real compiler's macro values, a clean parse).
export interface FileExpectation {
    /// Argument runs the resolved command must contain, in order and
    /// adjacent; `${root}` stands for the project copy.
    contains?: string[][];

    /// Arguments the resolved command must not contain.
    excludes?: string[];
}

export interface Scenario {
    /// Build system and toolchain, e.g. "cmake ninja gcc".
    name: string;

    platforms: NodeJS.Platform[];

    /// Executables the scenario runs, by name or absolute path.
    requires: string[];

    /// Build steps run in order in the project copy, each an argv.
    build: string[][];

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

/// Where `tool` resolves on the system PATH, undefined when it does not.
export function locate(tool: string): string | undefined {
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

/// Copy the shared project into a fresh directory. The caller removes it.
export function materialize(): string {
    const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), "clice-compat-")));
    fs.cpSync(PROJECT_DIR, root, { recursive: true });
    return root;
}

/// Run the scenario's build steps in `root`; throws with the failing
/// step's output.
export function build(scenario: Scenario, root: string): void {
    for (const [tool, ...args] of scenario.build) {
        if (tool === undefined) {
            throw new Error(`${scenario.name}: empty build step`);
        }
        const run = spawnSync(tool, args, {
            cwd: root,
            env: systemEnv(),
            encoding: "utf8",
            maxBuffer: 64 * 1024 * 1024,
        });
        if (run.error !== undefined || run.status !== 0) {
            throw new Error(
                `${scenario.name}: \`${[tool, ...args].join(" ")}\` failed ` +
                    `(${run.error?.message ?? `exit ${run.status}`})\n${run.stdout}\n${run.stderr}`,
            );
        }
    }
}
