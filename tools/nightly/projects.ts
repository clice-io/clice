/// The real projects of the nightly run, tests/nightly/projects.json: where
/// each comes from, pinned to a commit, how its compilation database is
/// made, and the parts a project too large for one runner is split into.
/// One job indexes one part, or the whole project when it has none.

import * as fs from "node:fs";
import * as path from "node:path";
import { REPO_ROOT } from "../repo.ts";

const PROJECTS_FILE = path.join(REPO_ROOT, "tests", "nightly", "projects.json");

/// A command run in the checkout, an argv in which `${JOBS}` reads the
/// machine's core count, `${CHECKOUT}` the checkout's path (with `/`) and an
/// argument `${CMAKE_TOOLCHAIN}` the CMake arguments of the platform's
/// target (prepare.ts); or the generated sources of a CMake build directory
/// using Ninja.
export type Step = string[] | { generated: string };

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
                `the nightly jobs run on Linux, macOS and Windows, not ${process.platform}`,
            );
    }
}

export interface Project {
    name: string;
    /// What the pin is, for a reader.
    about: string;
    /// The repository and the commit pinned; clice itself, at the commit
    /// under test, has neither.
    repository?: string;
    commit?: string;
    /// The platforms the project runs on, all of them when absent.
    platforms?: Platform[];
    /// The Debian packages the preparation needs on Linux.
    packages?: string[];
    prepare: Step[];
    /// The compilation database the preparation writes, checkout-relative.
    database: string;
    /// The parts, each the units under its checkout-relative prefixes.
    parts?: Record<string, string[]>;
    /// The files the feature pass samples every night.
    sample: number;
}

/// One job of the run: a project, and its part when it has parts.
export interface Job {
    name: string;
    project: Project;
    part?: string;
}

function fail(message: string): never {
    throw new Error(`${PROJECTS_FILE}: ${message}`);
}

export function loadProjects(): Project[] {
    const { projects } = JSON.parse(fs.readFileSync(PROJECTS_FILE, "utf8")) as {
        projects: Project[];
    };
    const names = new Set<string>();
    for (const project of projects) {
        if (names.has(project.name)) {
            fail(`two projects are named ${project.name}`);
        }
        names.add(project.name);
        if ((project.repository === undefined) !== (project.commit === undefined)) {
            fail(`${project.name} names a repository without a commit, or a commit without one`);
        }
        const unknown = (project.platforms ?? []).filter((each) => !PLATFORMS.includes(each));
        if (unknown.length > 0) {
            fail(`${project.name} names unknown platforms: ${unknown.join(", ")}`);
        }
        if (project.commit !== undefined && !/^[0-9a-f]{40}$/.test(project.commit)) {
            fail(`${project.name} pins ${project.commit}, not a full commit hash`);
        }
    }
    return projects;
}

/// Every job of the run, `<project>` or `<project>/<part>`.
export function jobs(projects: readonly Project[]): Job[] {
    return projects.flatMap((project) =>
        project.parts === undefined
            ? [{ name: project.name, project }]
            : Object.keys(project.parts).map((part) => ({
                  name: `${project.name}/${part}`,
                  project,
                  part,
              })),
    );
}

export function findJob(projects: readonly Project[], name: string): Job {
    const job = jobs(projects).find((each) => each.name === name);
    if (job === undefined) {
        throw new Error(
            `no nightly job ${name}; the jobs are ${jobs(projects)
                .map((each) => each.name)
                .join(", ")}`,
        );
    }
    return job;
}
