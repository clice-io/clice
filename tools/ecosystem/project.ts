/// The real projects of ecosystem/ (ecosystem/README.md): where each comes
/// from, pinned to a commit, and the configurations it is built in, each
/// with how its compilation database is made and the parts one too large
/// for a runner is split into.

import * as fs from "node:fs";
import * as path from "node:path";
import { parse } from "smol-toml";
import type { DatabaseEntry } from "../compat/database.ts";
import { REPO_ROOT } from "../repo.ts";

export const ECOSYSTEM = path.join(REPO_ROOT, "ecosystem");

/// A step of the preparation: `run`, a command run in the checkout, an argv
/// in which `${JOBS}` reads the machine's core count, `${CHECKOUT}` the
/// checkout's path (with `/`) and an argument `${CMAKE_TOOLCHAIN}` the CMake
/// arguments of the platform's target (prepare.ts); `generated`, the
/// generated sources of a CMake build directory using Ninja; or `database`,
/// a database written as given.
export type Step = { run: string[] } | { generated: string } | { database: DatabaseEntry[] };

const STEPS = ["run", "generated", "database"];

export const PLATFORMS = ["linux", "macos", "windows"] as const;
export type Platform = (typeof PLATFORMS)[number];

export function currentPlatform(): Platform {
    switch (process.platform) {
        case "linux":
            return "linux";
        case "darwin":
            return "macos";
        case "win32":
            return "windows";
        default:
            throw new Error(
                `the ecosystem jobs run on Linux, macOS and Windows, not ${process.platform}`,
            );
    }
}

export interface Configuration {
    /// What the configuration builds, for a reader.
    about: string;
    /// The platforms it builds on, all of them when absent.
    platforms?: Platform[];
    /// The Debian packages the preparation needs on Linux.
    packages?: string[];
    /// MSVC's environment, which its compiler needs: the preparation and the
    /// compiler's checks run in it, the servers never.
    environment?: "vcvars";
    prepare: Step[];
    /// The compilation database the preparation writes, checkout-relative.
    database: string;
    /// The parts, each the units under its checkout-relative prefixes.
    parts?: Record<string, string[]>;
    /// The units a database too heavy for one runner is thinned to, spread
    /// evenly over it in path order.
    keep?: number;
}

export interface Project {
    /// Its directory under ecosystem/.
    name: string;
    /// What the pin is, for a reader.
    about: string;
    /// The repository and the commit pinned, or an archive; clice itself, at
    /// the commit under test, has neither.
    repository?: string;
    commit?: string;
    archive?: { url: string; sha256: string };
    configurations: Record<string, Configuration>;
}

function fail(name: string, message: string): never {
    throw new Error(`ecosystem/${name}/project.toml: ${message}`);
}

export function validated(project: Project): Project {
    const { name } = project;
    if ((project.repository === undefined) !== (project.commit === undefined)) {
        fail(name, "names a repository without a commit, or a commit without one");
    }
    if (project.repository !== undefined && project.archive !== undefined) {
        fail(name, "names both a repository and an archive");
    }
    if (project.commit !== undefined && !/^[0-9a-f]{40}$/.test(project.commit)) {
        fail(name, `pins ${project.commit}, not a full commit hash`);
    }
    if (project.archive !== undefined && !/^[0-9a-f]{64}$/.test(project.archive.sha256)) {
        fail(name, `pins its archive by ${project.archive.sha256}, not a SHA-256`);
    }
    const configurations = Object.entries(project.configurations);
    if (configurations.length === 0) {
        fail(name, "has no configuration");
    }
    for (const [configuration, { platforms, prepare, parts, keep }] of configurations) {
        const where = `configurations.${configuration}`;
        const unknown = (platforms ?? []).filter((each) => !PLATFORMS.includes(each));
        if (unknown.length > 0) {
            fail(name, `${where}.platforms names unknown platforms: ${unknown.join(", ")}`);
        }
        for (const [index, step] of prepare.entries()) {
            const kinds = STEPS.filter((kind) => Object.hasOwn(step, kind));
            if (kinds.length !== 1) {
                fail(
                    name,
                    `${where}.prepare[${String(index)}] needs exactly one of run, generated and database, not ${kinds.join(" and ") || "none"}`,
                );
            }
        }
        if (parts !== undefined && keep !== undefined) {
            fail(name, `${where} both splits its database (parts) and thins it (keep)`);
        }
    }
    return project;
}

export function loadProject(name: string): Project {
    const file = path.join(ECOSYSTEM, name, "project.toml");
    if (!fs.existsSync(file)) {
        throw new Error(`no project ${name}: ${file} is missing`);
    }
    let project: Omit<Project, "name">;
    try {
        project = parse(fs.readFileSync(file, "utf8")) as unknown as Omit<Project, "name">;
    } catch (error) {
        fail(name, (error as Error).message);
    }
    return validated({ name, ...project });
}

/// Every project, by name.
export function loadProjects(): Project[] {
    return fs
        .readdirSync(ECOSYSTEM, { withFileTypes: true })
        .filter((entry) => entry.isDirectory() && entry.name !== "node_modules")
        .map((entry) => entry.name)
        .sort()
        .map(loadProject);
}

export function platformsOf(configuration: Configuration): readonly Platform[] {
    return configuration.platforms ?? PLATFORMS;
}

/// The configuration a job names, as its name shows it: left out when the
/// project has only one.
export function configurationPart(project: Project, configuration: string): string[] {
    return Object.keys(project.configurations).length === 1 ? [] : [configuration];
}

export function configurationOf(project: Project, configuration: string): Configuration {
    const found = project.configurations[configuration];
    if (found === undefined) {
        throw new Error(
            `${project.name} has no configuration ${configuration}; it has ${Object.keys(project.configurations).join(", ")}`,
        );
    }
    return found;
}
