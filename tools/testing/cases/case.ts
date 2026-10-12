/// The cases under tests-next/: `<topic>/<subtopic>/NN_name/`, each a small
/// project with what to check. Read strictly: a stray file, an unknown
/// key or a malformed page is an error, never a silent default.

import * as fs from "node:fs";
import * as path from "node:path";
import yaml from "js-yaml";
import { parseAnnotations, type AnnotatedSource } from "./annotation.ts";
import type { MarkerAnchor } from "./anchors.ts";
import { MARKED, inputFiles, mergeProjects, posix, readProject, type Project } from "./project.ts";
import { SUITES, TOPICS } from "./topics.ts";

/// A documented case's page: README.md.
export interface Readme {
    issues: string[];
    /// The configuration key whose effect the case shows.
    option?: string | undefined;
    title: string;
    summary: string;
    prose: string;
}

export interface Case {
    /// `<topic>/<subtopic>/<name>`.
    id: string;
    topic: string;
    subtopic: string;
    name: string;
    dir: string;
    project: Project;
    /// Input files as the case directory names them.
    inputs: string[];
    /// The markers of its sources, in path and source order.
    markers: MarkerAnchor[];
    readme: Readme | undefined;
    /// The script the case runs: its own test.ts, its subtopic's or its
    /// topic's.
    script: string;
}

const CASE = /^\d\d_[a-z0-9]+(_[a-z0-9]+)*$/;
const NAME = /^[a-z0-9]+(_[a-z0-9]+)*$/;
const README_KEYS = ["issues", "option"];

function fail(where: string, message: string): never {
    throw new Error(`${where}: ${message}`);
}

function subdirs(dir: string, where: string, allowed: readonly string[]): string[] {
    const out: string[] = [];
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (entry.isDirectory()) {
            out.push(entry.name);
        } else if (!allowed.includes(entry.name)) {
            fail(
                `${where}/${entry.name}`,
                `only ${allowed.join(" and ")} and directories belong here`,
            );
        }
    }
    return out.sort();
}

export function parseReadme(text: string, where: string): Readme {
    const match = /^(?:---\n([\s\S]*?)\n---\n)?([\s\S]*)$/.exec(text.replace(/\r\n/g, "\n"));
    const meta = (yaml.load(match?.[1] ?? "") ?? {}) as Record<string, unknown>;
    for (const key of Object.keys(meta)) {
        if (!README_KEYS.includes(key)) {
            fail(where, `unknown key '${key}': a page's --- block holds issues and option`);
        }
    }
    const issues = meta["issues"] ?? [];
    if (!Array.isArray(issues) || !issues.every((issue) => typeof issue === "string")) {
        fail(where, "issues is a list");
    }
    const option = meta["option"];
    if (option !== undefined && typeof option !== "string") {
        fail(where, "option names a configuration key");
    }
    const body = (match?.[2] ?? "").trim().split(/\n\s*\n/);
    const heading = body[0] ?? "";
    if (!heading.startsWith("# ") || heading.includes("\n")) {
        fail(where, "the page's first line is its title: '# Name'");
    }
    const title = heading.slice(2).trim();
    if (title.split(/\s+/).length > 5 || /[.!?:]$/.test(title)) {
        fail(where, "a title has at most five words and no final punctuation");
    }
    const summary = (body[1] ?? "").replace(/\n/g, " ").trim();
    if (summary === "" || summary.endsWith(".")) {
        fail(where, "the summary is one sentence after the title, without a final period");
    }
    return {
        issues,
        option,
        title,
        summary,
        prose: body.slice(2).join("\n\n"),
    };
}

/// UTF-16 offset of byte offset `byte` in `text`.
function utf16(text: string, byte: number): number {
    return Buffer.from(text).subarray(0, byte).toString().length;
}

/// The markers of the files, in path and source order; unnamed ones are
/// numbered across the case, and a name is unique across it.
function markersOf(id: string, marked: ReadonlyMap<string, AnnotatedSource>): MarkerAnchor[] {
    const out: MarkerAnchor[] = [];
    const owners = new Map<string, string>();
    let nameless = 0;
    for (const [file, source] of [...marked].sort(([a], [b]) => (a < b ? -1 : 1))) {
        const text = source.content;
        const found: { name: string; begin: number; end: number; range: boolean }[] = [
            ...[...source.offsets].map(([name, at]) => ({
                name,
                begin: at,
                end: at,
                range: false,
            })),
            ...source.namelessOffsets.map((at) => ({ name: "", begin: at, end: at, range: false })),
            ...[...source.ranges].map(([name, [begin, end]]) => ({
                name,
                begin,
                end,
                range: true,
            })),
        ];
        found.sort((a, b) => a.begin - b.begin || b.end - a.end);
        for (const marker of found) {
            let name = marker.name;
            if (name === "") {
                name = `nameless_${String(nameless)}`;
                nameless += 1;
            } else {
                const other = owners.get(name);
                if (other !== undefined) {
                    fail(`${id}/${file}`, `§(${name}) is also in ${other}: names are unique`);
                }
                owners.set(name, file);
            }
            out.push({
                path: file,
                name,
                begin: utf16(text, marker.begin),
                end: utf16(text, marker.end),
                range: marker.range,
                text,
            });
        }
    }
    return out;
}

function readCase(
    root: string,
    topic: string,
    subtopic: string,
    name: string,
    outer: { projects: Project[]; scripts: string[] },
): Case {
    const dir = path.join(root, topic, subtopic, name);
    const id = `${topic}/${subtopic}/${name}`;
    const inputs = inputFiles(dir);
    const marked = new Map<string, AnnotatedSource>();
    for (const rel of inputs) {
        const text = fs.readFileSync(path.join(dir, rel), "utf8");
        if (rel.endsWith(".in") && text.includes("§")) {
            fail(`${id}/${rel}`, "a .in file carries no markers");
        }
        if (MARKED.test(rel)) {
            const source = parseAnnotations(text);
            if (source.content !== text) {
                marked.set(rel, source);
            }
        } else if (text.includes("§")) {
            fail(`${id}/${rel}`, "only C-family sources carry markers");
        }
    }
    const markers = markersOf(id, marked);
    const readmePath = path.join(dir, "README.md");
    const readme = fs.existsSync(readmePath)
        ? parseReadme(fs.readFileSync(readmePath, "utf8"), `${id}/README.md`)
        : undefined;
    const own = path.join(dir, "test.ts");
    const script = fs.existsSync(own) ? own : outer.scripts.at(-1);
    if (script === undefined) {
        fail(id, "the case needs a test.ts, here or above");
    }
    return {
        id,
        topic,
        subtopic,
        name,
        dir,
        project: mergeProjects([...outer.projects, readProject(dir)]),
        inputs,
        markers,
        readme,
        script,
    };
}

/// Every case under `root`, in path order.
export function loadCases(root: string): Case[] {
    const cases: Case[] = [];
    const outerScripts: string[] = [];
    const scriptIn = (dir: string): string[] =>
        fs.existsSync(path.join(dir, "test.ts")) ? [path.join(dir, "test.ts")] : [];
    for (const topic of fs.readdirSync(root, { withFileTypes: true })) {
        if (!topic.isDirectory() || SUITES.has(topic.name)) {
            continue;
        }
        const topicDir = path.join(root, topic.name);
        if (!Object.hasOwn(TOPICS, topic.name)) {
            fail(topic.name, "not a topic: topics.ts lists them, as tests-next/README.md does");
        }
        for (const subtopic of subdirs(topicDir, topic.name, ["project.jsonc", "test.ts"])) {
            const subDir = path.join(topicDir, subtopic);
            const where = `${topic.name}/${subtopic}`;
            if (!NAME.test(subtopic) || subtopic === topic.name) {
                fail(
                    where,
                    "a subtopic names an aspect of its topic in lowercase words joined by _",
                );
            }
            const outer = {
                projects: [readProject(topicDir), readProject(subDir)],
                scripts: [...scriptIn(topicDir), ...scriptIn(subDir)],
            };
            outerScripts.push(...outer.scripts);
            for (const name of subdirs(subDir, where, ["project.jsonc", "test.ts"])) {
                if (!CASE.test(name)) {
                    fail(
                        `${where}/${name}`,
                        "a case is named NN_words: two digits, lowercase words joined by _",
                    );
                }
                cases.push(readCase(root, topic.name, subtopic, name, outer));
            }
        }
    }
    const run = new Set(cases.map((c) => c.script));
    for (const script of new Set(outerScripts)) {
        if (!run.has(script)) {
            fail(
                posix(path.relative(root, script)),
                "no case runs this script: every case below has its own",
            );
        }
    }
    return cases.sort((a, b) => (a.id < b.id ? -1 : 1));
}

/// Shard `index` of `count` (`<index>/<count>`, 1-based) of `cases`: whole
/// subtopics, dealt largest first to the shard with the fewest cases so far.
export function shard(cases: readonly Case[], spec: string): Case[] {
    const match = /^(\d+)\/(\d+)$/.exec(spec);
    const index = Number(match?.[1]);
    const count = Number(match?.[2]);
    if (match === null || index < 1 || index > count) {
        fail("CLICE_CASES_SHARD", `'${spec}' is not <index>/<count>, counted from 1`);
    }
    const subtopics = new Map<string, Case[]>();
    for (const c of cases) {
        const key = `${c.topic}/${c.subtopic}`;
        subtopics.set(key, [...(subtopics.get(key) ?? []), c]);
    }
    const loads = Array.from({ length: count }, () => 0);
    const mine: Case[] = [];
    const largest = [...subtopics].sort(
        ([a, x], [b, y]) => y.length - x.length || (a < b ? -1 : 1),
    );
    for (const [, group] of largest) {
        const target = loads.indexOf(Math.min(...loads));
        loads[target] = (loads[target] ?? 0) + group.length;
        if (target === index - 1) {
            mine.push(...group);
        }
    }
    return mine.sort((a, b) => (a.id < b.id ? -1 : 1));
}
